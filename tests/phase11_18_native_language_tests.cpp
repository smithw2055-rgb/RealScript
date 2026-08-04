#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/Gameplay.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string diagnosticsText(const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        if (!result.empty()) result.push_back('\n');
        result += diagnostic.code + ": " + diagnostic.message;
    }
    return result;
}

std::vector<realscript::bytecode::Module> compileModules(
    std::vector<realscript::compiler::SourceFile> files) {
    realscript::compiler::Compilation compilation(std::move(files));
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "native source compilation failed:\n" + diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& mir : build.modules) {
        auto module = lowerer.lower(mir);
        realscript::diagnostics::DiagnosticBag diagnostics;
        const auto verified =
            realscript::bytecode::verifyModule(module, diagnostics);
        require(verified,
            "extended bytecode verification failed:\n" +
                diagnosticsText(diagnostics));
        modules.push_back(std::move(module));
    }
    return modules;
}

void testPhase11To15And17Execution() {
    const char* source = R"(
module Extended;

delegate void ChangedHandler(int amount);

interface ICounter
{
    int Read();
}

[Serializable(version = 2)]
class Box<T> : ICounter
{
    T value;
    int total;
    event ChangedHandler Changed;

    Box(T initial)
    {
        value = initial;
        total = 0;
    }

    void OnChanged(int amount)
    {
        total = total + amount;
    }

    int Read()
    {
        return total;
    }

    int Run(int[] values)
    {
        Changed += OnChanged;
        Changed += amount => total = total + amount;

        for (int index = 0; index < values.length; index = index + 1)
        {
            if (values[index] < 0) continue;
            if (values[index] == 99) break;
            Changed(values[index]);
        }

        List<int> list = new List<int>(8);
        foreach (int item in values)
        {
            list.Add(item);
        }
        foreach (int item in list)
        {
            if (item > 0)
            {
                total = total + item;
            }
        }

        switch (list.Count())
        {
            case 3:
                total = total + 1;
                break;
            default:
                total = total + 100;
                break;
        }
        return total;
    }
}

T Identity<T>(T value)
{
    return value;
}

void Bump(ref int value, out int doubled, in int amount)
{
    value = value + amount;
    doubled = value + value;
}

int main()
{
    int[] values = new int[3];
    values[0] = 1;
    values[1] = -2;
    values[2] = 3;

    Box<int> box = new Box<int>(0);
    int result = box.Run(values);
    int value = 1;
    int doubled = 0;
    Bump(ref value, out doubled, in 2);
    return result + value + doubled + Identity<int>(1);
}
)";

    auto modules = compileModules({{"extended.rs", source}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Extended::main");
    require(result.succeeded, "extended main execution failed: " + result.error.message);
    require(std::get<std::int64_t>(result.value) == 23,
        "extended main returned the wrong result");
}

void testNestedSwitchControlFlow() {
    const char* source = R"(
module ControlEdges;

int main()
{
    int total = 0;
    int index = 0;
    while (index < 4)
    {
        index = index + 1;
        switch (index)
        {
            default:
                total = total + 100;
                break;
            case 2:
                continue;
            case 3:
                total = total + 3;
                break;
        }
        total = total + 1;
    }
    return total;
}
)";

    auto modules = compileModules({{"control_edges.rs", source}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("ControlEdges::main");
    require(result.succeeded,
        "nested switch execution failed: " + result.error.message);
    require(std::get<std::int64_t>(result.value) == 206,
        "nested switch/default/continue semantics were incorrect");
}

void testCrossFileDeclarationSharing() {
    const char* contracts = R"(
module MultiFile;

delegate void ChangedHandler(int amount);

interface ICounter
{
    int Read();
}

T Identity<T>(T value)
{
    return value;
}

void Bump(ref int value)
{
    value = value + 1;
}
)";

    const char* model = R"(
module MultiFile;

class Counter : ICounter
{
    int total;
    event ChangedHandler Changed;

    void OnChanged(int amount)
    {
        total = total + amount;
    }

    int Read()
    {
        return total;
    }

    int Run()
    {
        Changed += OnChanged;
        Changed(5);
        return total;
    }
}
)";

    const char* appA = R"(
module MultiFile;

int ApplyRef(int value)
{
    Bump(ref value);
    return value;
}

int main()
{
    Counter counter = new Counter();
    return counter.Run() + Identity<int>(2) + Identity<int>(3) +
        ApplyRef(4) + Other();
}
)";

    const char* appB = R"(
module MultiFile;

int Other()
{
    int value = 1;
    Bump(ref value);
    return Identity<int>(value);
}
)";

    auto modules = compileModules({
        {"01_contracts.rs", contracts},
        {"02_model.rs", model},
        {"03_app_a.rs", appA},
        {"04_app_b.rs", appB},
    });
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("MultiFile::main");
    require(result.succeeded,
        "multi-file expansion execution failed: " + result.error.message);
    require(std::get<std::int64_t>(result.value) == 17,
        "multi-file expansion returned the wrong result");
}

void testCrossModuleImportsAndIsolation() {
    const char* contracts = R"(
module Shared.Contracts;

delegate void ChangedHandler(int amount);

interface IReader
{
    int Read();
}

T Identity<T>(T value)
{
    return value;
}

void Increment(ref int value)
{
    value = value + 1;
}
)";

    const char* application = R"(
module Imported.App;
import Shared.Contracts;

class Counter : IReader
{
    int total;
    event ChangedHandler Changed;

    void Add(int amount)
    {
        total = total + amount;
    }

    int Read()
    {
        return total;
    }

    int Run()
    {
        Changed += Add;
        Changed(5);
        return total;
    }
}

int main()
{
    Counter counter = new Counter();
    int value = 1;
    Increment(ref value);
    return counter.Run() + Identity<int>(2) + value;
}
)";

    const char* isolated = R"(
module Other.Contracts;

T Identity<T>(T value)
{
    return value + value;
}

int other()
{
    return Identity<int>(3);
}
)";

    auto modules = compileModules({
        {"01_shared_contracts.rs", contracts},
        {"02_imported_app.rs", application},
        {"03_other_contracts.rs", isolated},
    });
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto imported = interpreter.invoke("Imported.App::main");
    require(imported.succeeded,
        "cross-module language expansion failed: " + imported.error.message);
    require(std::get<std::int64_t>(imported.value) == 9,
        "imported declarations produced the wrong result");

    const auto other = interpreter.invoke("Other.Contracts::other");
    require(other.succeeded,
        "isolated generic module failed: " + other.error.message);
    require(std::get<std::int64_t>(other.value) == 6,
        "same-name generic declarations leaked across modules");
}

void testNativeMetadata() {
    realscript::compiler::Compilation compilation({{
        "metadata.rs",
        "module Meta; [Replicated(channel = \"state\")] "
        "class Unit { int health; }"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "native metadata compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    require(build.nativeAttributes.size() == 1,
        "native source attribute was not captured");
    require(build.nativeAttributes.front().target == "Meta::Unit",
        "source attribute target was not module-qualified");
    require(build.nativeAttributes.front().name == "Replicated",
        "source attribute name changed");
    require(build.nativeAttributes.front().arguments.size() == 1 &&
            build.nativeAttributes.front().arguments.front().name ==
                "channel" &&
            build.nativeAttributes.front().arguments.front().value ==
                "\"state\"",
        "source attribute arguments were not captured");
}


void testNativeAotGeneration() {
    const char* source = R"(
module ExpandedAot;

T Identity<T>(T value)
{
    return value;
}

int main()
{
    List<int> values = new List<int>(4);
    for (int index = 0; index < 3; index = index + 1)
    {
        values.Add(index + 1);
    }
    int total = 0;
    foreach (int value in values)
    {
        total = total + value;
    }
    return Identity<int>(total);
}
)";

    realscript::compiler::Compilation compilation({{"expanded_aot.rs", source}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "native AOT source failed to compile:\n" +
        diagnosticsText(build.diagnostics));

    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::aot::CppGenerator generator;
    realscript::aot::GenerationOptions options;
    options.programName = "Phase11To17Expanded";
    const auto generated = generator.generate(build.modules, diagnostics, options);
    require(!diagnostics.hasErrors() && generated.contentHash != 0 &&
            generated.source.find("Phase11To17ExpandedProgram") !=
                std::string::npos,
        "native MIR did not generate deterministic C++17 AOT source");
}

void testNativeMetadataArtifacts() {
    const char* source = R"(
module ArtifactMeta;

interface IRead { int Read(); }

[Serializable(version = 3)]
class Box<T> : IRead
{
    T value;
    Box(T initial) { value = initial; }
    int Read() { return 1; }
}

T Identity<T>(T value) { return value; }

int main()
{
    Box<int> value = new Box<int>(1);
    return Identity<int>(value.Read());
}
)";

    realscript::compiler::Compilation compilation({{"artifact_meta.rs", source}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "metadata artifact source failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    require(build.modules.size() == 1 &&
            !build.modules.front().languageMetadata.attributes.empty() &&
            !build.modules.front().languageMetadata.interfaces.empty() &&
            build.modules.front().languageMetadata.genericInstantiations.size() >= 2,
        "MIR module did not retain native language metadata");

    realscript::bytecode::Lowerer lowerer;
    const auto bytecode = lowerer.lower(build.modules.front());
    require(bytecode.version.major == 0 && bytecode.version.minor == 7,
        "native object metadata did not advance the RSBC format to 0.7");
    const auto bytes = realscript::bytecode::encodeModule(bytecode);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag decodeDiagnostics;
    require(realscript::bytecode::decodeModule(
                bytes, decoded, decodeDiagnostics) &&
            !decodeDiagnostics.hasErrors(),
        "RSBC metadata round trip failed:\n" +
            diagnosticsText(decodeDiagnostics));
    require(decoded.languageMetadata.attributes.size() ==
                bytecode.languageMetadata.attributes.size() &&
            decoded.languageMetadata.interfaces.size() ==
                bytecode.languageMetadata.interfaces.size() &&
            decoded.languageMetadata.genericInstantiations.size() ==
                bytecode.languageMetadata.genericInstantiations.size() &&
            decoded.languageMetadata.attributes.front().name == "Serializable" &&
            decoded.languageMetadata.genericInstantiations.front().generatedName.find("__int") !=
                std::string::npos,
        "decoded RSBC language metadata changed");

    realscript::aot::CppGenerator generator;
    realscript::aot::GenerationOptions options;
    options.programName = "Phase18Metadata";
    realscript::diagnostics::DiagnosticBag aotDiagnostics;
    const auto generated = generator.generate(
        build.modules, aotDiagnostics, options);
    require(!aotDiagnostics.hasErrors() &&
            generated.manifest.find("\"languageMetadata\"") !=
                std::string::npos &&
            generated.manifest.find("Serializable") != std::string::npos &&
            generated.manifest.find("Box__int") != std::string::npos &&
            generated.manifest.find("Identity__int") != std::string::npos,
        "AOT manifest did not retain native language metadata");
}

void testPhase16DeterministicSequence() {
    realscript::game::GameApi api;
    auto host = std::make_shared<realscript::game::GameplayHost>(30, 5, 7);
    require(realscript::game::installGameplayBindings(api, host),
        "gameplay bindings failed");

    const char* source = R"(
module SequenceDemo;
import RealScript.Game;

[Behavior(category = "combat")]
class Behavior
{
    int total;

    sequence Attack(long target)
    {
        total = 1;
        yield wait_ticks(1);
        total = total + 2;
        yield wait_ticks(1);
        total = total + 3;
    }

    int Read()
    {
        return total;
    }
}
)";

    realscript::game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"sequence.rs", source}});
    require(compiled.succeeded(),
        "sequence source compilation failed:\n" + diagnosticsText(compiled.diagnostics));
    require(compiled.languageMetadata.attributes.size() == 1 &&
            compiled.languageMetadata.attributes.front().target ==
                "SequenceDemo::Behavior",
        "GameCompileResult did not retain source attributes");
    require(compiled.program.languageMetadata().attributes.size() == 1,
        "GameProgram did not retain source attributes");
    require(compiled.languageMetadata.sequences.size() == 1 &&
            compiled.languageMetadata.sequences.front().typeName ==
                "SequenceDemo::Behavior" &&
            compiled.languageMetadata.sequences.front().name == "Attack" &&
            compiled.languageMetadata.sequences.front().callbacks.size() == 1 &&
            compiled.languageMetadata.sequences.front().resultTypeName == "void",
        "GameCompileResult did not retain native sequence metadata");
    require(compiled.program.languageMetadata().sequences.size() == 1,
        "GameProgram did not retain native sequence metadata");

    realscript::game::ScriptRuntime scripts(compiled.program);
    realscript::game::SceneScriptRuntime scene(scripts);
    require(scene.attach(10, "SequenceDemo::Behavior"),
        "sequence behavior attachment failed");
    scene.start();

    const auto started = scene.invoke(10, "Attack", {realscript::runtime::LongValue{10}});
    require(started.succeeded, "sequence start failed: " + started.error.message);

    realscript::game::SceneGameplayDriver driver(scene, host);
    const auto steps = driver.advanceTicks(2);
    require(steps.size() == 2, "sequence driver did not advance two ticks");
    require(driver.errors().empty(), "sequence driver recorded callback errors");

    const auto read = scene.invoke(10, "Read");
    require(read.succeeded && std::get<std::int64_t>(read.value) == 6,
        "deterministic sequence produced the wrong state");
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
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };

    run("phase 11-15 and 17 execution", testPhase11To15And17Execution);
    run("nested switch control flow", testNestedSwitchControlFlow);
    run("cross-file declaration sharing", testCrossFileDeclarationSharing);
    run("cross-module imports and isolation", testCrossModuleImportsAndIsolation);
    run("native source metadata", testNativeMetadata);
    run("AOT generation from native source", testNativeAotGeneration);
    run("native metadata artifacts", testNativeMetadataArtifacts);
    run("phase 16 deterministic sequence", testPhase16DeterministicSequence);
    return failures == 0 ? 0 : 1;
}
