#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<realscript::bytecode::Module> compile(
    const std::vector<realscript::compiler::SourceFile>& files) {
    realscript::compiler::Compilation compilation;
    for (const auto& file : files) compilation.addSource(file);
    auto result = compilation.build();
    require(!result.diagnostics.hasErrors(), "source compilation failed");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& mir : result.modules) modules.push_back(lowerer.lower(mir));
    return modules;
}

void testProgramImageLinksOnce() {
    realscript::runtime::RuntimeError error;
    auto image = realscript::runtime::ProgramImage::link(
        compile({{"main.rs", "module Demo; int main() { return 42; }"}}), error);
    require(image.has_value(), "program image linking failed");
    require(image->moduleCount() == 1, "unexpected module count");
    require(image->functionCount() == 1, "unexpected function count");
    require(image->findFunction("Demo::main").has_value(), "linked function not indexed");
}

void testRuntimeImageIndexesSurviveMove() {
    auto modules = compile({{"indexed.rs", R"(
module Demo.Indexed;
class Box
{
    int value;
    Box(int initial) { value = initial; }
    int Read() { return value; }
}
int main() { Box box = new Box(42); return box.Read(); }
)"}});
    realscript::runtime::RuntimeError error;
    auto image = realscript::runtime::ProgramImage::link(
        std::move(modules), error);
    require(image.has_value(),
        "indexed runtime image failed to link: " + error.message);
    const auto entry = image->findFunction("Demo.Indexed::main");
    require(entry.has_value(), "indexed runtime image lost the entry name");
    const auto* beforeMove = image->resolveFunction(*entry);
    require(beforeMove && beforeMove->module && beforeMove->function &&
            beforeMove->module->name == "Demo.Indexed" &&
            beforeMove->function->name == "main",
        "runtime function index does not resolve its bytecode location");
    const auto boxTypeId = realscript::semantic::stableTypeId(
        "Demo.Indexed::Box");
    require(image->resolveType(boxTypeId) != nullptr,
        "runtime type index does not resolve the linked class");

    realscript::runtime::ProgramImage copied(*image);
    const auto* afterCopy = copied.resolveFunction(*entry);
    require(afterCopy && afterCopy->module && afterCopy->function &&
            afterCopy->function != beforeMove->function &&
            copied.resolveType(boxTypeId) != nullptr,
        "copied ProgramImage did not rebuild its pointer indexes");

    auto shared = std::make_shared<realscript::runtime::ProgramImage>(
        std::move(copied));
    const auto* afterMove = shared->resolveFunction(*entry);
    require(afterMove && afterMove->module && afterMove->function &&
            shared->resolveType(boxTypeId) != nullptr,
        "runtime indexes became invalid after ProgramImage move");
    realscript::runtime::EngineRuntime runtime(shared);
    const auto result = runtime.invoke(*entry);
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "SymbolId-directed EngineRuntime invocation failed");
}

void testDuplicateSymbolsRejected() {
    auto modules = compile({{"main.rs", "module Demo; int main() { return 1; }"}});
    auto duplicate = modules.front();
    duplicate.name = "Other";
    realscript::runtime::RuntimeError error;
    auto image = realscript::runtime::ProgramImage::link({modules.front(), duplicate}, error);
    require(!image.has_value(), "duplicate SymbolId must be rejected");
    require(error.code == realscript::runtime::ErrorCode::DuplicateSymbol, "wrong duplicate error");
}

void testQualifiedOverloadsLinkBySymbolIdentity() {
    auto modules = compile({{"overloads.rs", R"(
module Demo;
int Pick(int value) { return value; }
int Pick(bool value) { if (value) return 2; return 0; }
int main() { return Pick(1) + Pick(true); }
)"}});
    realscript::runtime::RuntimeError error;
    auto image = realscript::runtime::ProgramImage::link(
        std::move(modules), error);
    require(image.has_value(),
        "qualified overloads failed to link: " + error.message);
    require(image->functionCount() == 3,
        "qualified overload link dropped a symbol");
    require(image->findFunction("Demo::Pick").has_value(),
        "legacy overload name index is missing");
    realscript::runtime::EngineRuntime runtime(
        std::make_shared<realscript::runtime::ProgramImage>(
            std::move(*image)));
    const auto result = runtime.invoke("Demo::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 3,
        "SymbolId-directed overload calls produced the wrong result");
}

void testBindingRegistryByName() {
    realscript::bytecode::Module module;
    module.name = "Demo";
    realscript::bytecode::FunctionReference reference;
    reference.symbolId = 7001;
    reference.name = "Host::answer";
    reference.returnType = realscript::semantic::PrimitiveType::Int;
    module.functionReferences.push_back(reference);
    realscript::bytecode::Function function;
    function.symbolId = 1;
    function.name = "main";
    function.returnType = realscript::semantic::PrimitiveType::Int;
    function.registerTypes = {realscript::semantic::PrimitiveType::Int};
    function.registerTypeIds = {0};
    realscript::bytecode::BasicBlock block; block.id = 0;
    realscript::bytecode::Instruction call;
    call.opcode = realscript::bytecode::Opcode::Call;
    call.result = 0;
    call.index = 0;
    block.instructions.push_back(call);
    block.terminator.kind = realscript::bytecode::TerminatorKind::ReturnValue;
    block.terminator.value = 0;
    function.blocks.push_back(block);
    module.functions.push_back(function);

    realscript::runtime::RuntimeError error;
    auto image = realscript::runtime::ProgramImage::link({module}, error);
    require(image.has_value(), "host-call image failed to link");
    auto bindings = std::make_shared<realscript::runtime::BindingRegistry>();
    require(bindings->bind("Host::answer", [](const auto&, const auto&, auto&) {
        return std::optional<realscript::runtime::Value>{std::int64_t{42}};
    }), "host binding failed");
    realscript::runtime::EngineRuntime runtime(
        std::make_shared<realscript::runtime::ProgramImage>(std::move(*image)));
    runtime.setBindings(bindings);
    const auto result = runtime.invoke("Demo::main");
    require(result.succeeded && std::get<std::int64_t>(result.value) == 42,
        "registered host function did not execute");
}

void testTraceAndStatistics() {
    realscript::runtime::RuntimeError error;
    auto image = realscript::runtime::ProgramImage::link(
        compile({{"trace.rs", "module Demo; int twice(int x) { return x * 2; } int main() { return twice(21); }"}}), error);
    require(image.has_value(), "trace image failed to link");
    realscript::runtime::EngineRuntime runtime(
        std::make_shared<realscript::runtime::ProgramImage>(std::move(*image)));
    std::vector<realscript::runtime::TraceEvent> events;
    realscript::runtime::ExecutionOptions options;
    options.trace = [&](const auto& event) { events.push_back(event); };
    const auto result = runtime.invoke("Demo::main", {}, options);
    require(result.succeeded, "traced execution failed");
    require(result.statistics.functionCalls == 2, "function-call statistics incorrect");
    require(result.statistics.maximumCallDepth == 2, "maximum call depth incorrect");
    require(result.statistics.instructionsExecuted == result.instructionsExecuted,
        "instruction statistics diverged");
    require(!events.empty(), "trace did not emit events");
    require(events.front().kind == realscript::runtime::TraceEventKind::FunctionEnter,
        "first trace event must enter the entry function");
}

void testBranchStatisticsDeterministic() {
    auto modules = compile({{"flow.rs", "module Demo; int main() { int x = 3; while (x > 0) x = x - 1; return x; }"}});
    realscript::runtime::RuntimeError error;
    auto image = realscript::runtime::ProgramImage::link(std::move(modules), error);
    require(image.has_value(), "flow image failed to link");
    auto shared = std::make_shared<realscript::runtime::ProgramImage>(std::move(*image));
    realscript::runtime::EngineRuntime runtime(shared);
    const auto first = runtime.invoke("Demo::main");
    const auto second = runtime.invoke("Demo::main");
    require(first.succeeded && second.succeeded, "flow execution failed");
    require(first.statistics.branchesTaken > 0, "branch statistics were not recorded");
    require(first.statistics.instructionsExecuted == second.statistics.instructionsExecuted,
        "statistics are not deterministic");
    require(first.statistics.branchesTaken == second.statistics.branchesTaken,
        "branch counts are not deterministic");
}
}

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };
    run("Program image links once", testProgramImageLinksOnce);
    run("Runtime image indexes survive move",
        testRuntimeImageIndexesSurviveMove);
    run("Duplicate symbols rejected", testDuplicateSymbolsRejected);
    run("Qualified overloads link by SymbolId",
        testQualifiedOverloadsLinkBySymbolIdentity);
    run("Binding registry by name", testBindingRegistryByName);
    run("Trace and statistics", testTraceAndStatistics);
    run("Branch statistics deterministic", testBranchStatisticsDeterministic);
    return failures == 0 ? 0 : 1;
}
