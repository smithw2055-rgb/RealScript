#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<realscript::bytecode::Module> compile(const std::vector<realscript::compiler::SourceFile>& files) {
    realscript::compiler::Compilation compilation;
    for (const auto& file : files) compilation.addSource(file);
    auto result = compilation.build();
    require(!result.diagnostics.hasErrors(), "source compilation failed");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& mir : result.modules) {
        auto module = lowerer.lower(mir);
        realscript::diagnostics::DiagnosticBag diagnostics;
        require(realscript::bytecode::verifyModule(module, diagnostics), "bytecode verification failed");
        modules.push_back(std::move(module));
    }
    return modules;
}

void testArithmeticAndCalls() {
    auto modules = compile({{"math.rs", "module Demo; int twice(int x) { return x * 2; } int main() { return twice(21) + 1; }"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Demo::main");
    require(result.succeeded, "main execution failed");
    require(std::get<std::int64_t>(result.value) == 43, "unexpected arithmetic result");
}

void testControlFlowAndBlockArguments() {
    auto modules = compile({{"flow.rs", "module Demo; bool guarded(bool enabled, int value) { return enabled && value > 0; }"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    auto falseResult = interpreter.invoke("Demo::guarded", {false, std::int64_t{5}});
    auto trueResult = interpreter.invoke("Demo::guarded", {true, std::int64_t{5}});
    require(falseResult.succeeded && !std::get<bool>(falseResult.value), "short circuit false path failed");
    require(trueResult.succeeded && std::get<bool>(trueResult.value), "short circuit true path failed");
}

void testLoop() {
    auto modules = compile({{"loop.rs", "module Demo; int count(int value) { while (value > 0) value = value - 1; return value; }"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Demo::count", {std::int64_t{8}});
    require(result.succeeded && std::get<std::int64_t>(result.value) == 0, "loop execution failed");
}

void testDivisionTrap() {
    auto modules = compile({{"trap.rs", "module Demo; int divide(int a, int b) { return a / b; }"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Demo::divide", {std::int64_t{1}, std::int64_t{0}});
    require(!result.succeeded, "division by zero must fail");
    require(result.error.code == realscript::runtime::ErrorCode::DivisionByZero, "wrong division error");
}

void testInstructionBudget() {
    auto modules = compile({{"budget.rs", "module Demo; int forever() { while (true) { } }"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    realscript::runtime::Limits limits;
    limits.instructionBudget = 20;
    const auto result = interpreter.invoke("Demo::forever", {}, limits);
    require(!result.succeeded, "infinite loop must hit budget");
    require(result.error.code == realscript::runtime::ErrorCode::InstructionBudgetExceeded, "wrong budget error");
}

void testRecursionLimit() {
    auto modules = compile({{"rec.rs", "module Demo; int recurse(int value) { return recurse(value); }"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    realscript::runtime::Limits limits;
    limits.recursionLimit = 8;
    const auto result = interpreter.invoke("Demo::recurse", {std::int64_t{1}}, limits);
    require(!result.succeeded, "recursive function must hit recursion limit");
    require(result.error.code == realscript::runtime::ErrorCode::RecursionLimitExceeded, "wrong recursion error");
    require(!result.error.stackTrace.empty(), "runtime stack trace must be recorded");
}

void testCrossModuleCall() {
    auto modules = compile({
        {"math.rs", "module Game.Math; int twice(int value) { return value * 2; }"},
        {"main.rs", "module Game.Main; import Game.Math; int main() { return twice(20); }"},
    });
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Game.Main::main");
    require(result.succeeded && std::get<std::int64_t>(result.value) == 40, "cross-module call failed");
}

void testExternalResolver() {
    realscript::bytecode::Module module;
    module.name = "Host";
    realscript::bytecode::FunctionReference reference;
    reference.symbolId = 9001;
    reference.name = "Host::increment";
    reference.returnType = realscript::semantic::PrimitiveType::Int;
    reference.parameterTypes = {realscript::semantic::PrimitiveType::Int};
    reference.parameterTypeIds = {0};
    module.functionReferences.push_back(reference);

    realscript::bytecode::Function function;
    function.symbolId = 1;
    function.name = "main";
    function.returnType = realscript::semantic::PrimitiveType::Int;
    function.registerTypes = {realscript::semantic::PrimitiveType::Int, realscript::semantic::PrimitiveType::Int};
    function.registerTypeIds = {0, 0};
    realscript::bytecode::BasicBlock block;
    block.id = 0;
    realscript::bytecode::Instruction constant;
    constant.opcode = realscript::bytecode::Opcode::ConstantInt;
    constant.result = 0;
    constant.integerImmediate = 41;
    block.instructions.push_back(constant);
    realscript::bytecode::Instruction call;
    call.opcode = realscript::bytecode::Opcode::Call;
    call.result = 1;
    call.index = 0;
    call.operands = {0};
    block.instructions.push_back(call);
    block.terminator.kind = realscript::bytecode::TerminatorKind::ReturnValue;
    block.terminator.value = 1;
    function.blocks.push_back(block);
    module.functions.push_back(function);

    realscript::runtime::Interpreter interpreter({module});
    interpreter.setExternalResolver([](const auto&, const auto& args, auto&) -> std::optional<realscript::runtime::Value> {
        return std::get<std::int64_t>(args.front()) + 1;
    });
    const auto result = interpreter.invoke(realscript::semantic::SymbolId{1});
    require(result.succeeded && std::get<std::int64_t>(result.value) == 42, "external resolver failed");
}

void testIntegerOverflow() {
    auto modules = compile({{"overflow.rs", "module Demo; int add(int a, int b) { return a + b; }"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Demo::add", {std::int64_t{2147483647}, std::int64_t{1}});
    require(!result.succeeded, "overflow must fail");
    require(result.error.code == realscript::runtime::ErrorCode::IntegerOverflow, "wrong overflow error");
}

void testUnresolvedExternal() {
    realscript::bytecode::Module module;
    module.name = "Host";
    realscript::bytecode::FunctionReference reference;
    reference.symbolId = 9002;
    reference.name = "Host::missing";
    reference.returnType = realscript::semantic::PrimitiveType::Int;
    module.functionReferences.push_back(reference);
    realscript::bytecode::Function function;
    function.symbolId = 2;
    function.name = "main";
    function.returnType = realscript::semantic::PrimitiveType::Int;
    function.registerTypes = {realscript::semantic::PrimitiveType::Int};
    function.registerTypeIds = {0};
    realscript::bytecode::BasicBlock block; block.id = 0;
    realscript::bytecode::Instruction call; call.opcode = realscript::bytecode::Opcode::Call; call.result = 0; call.index = 0;
    block.instructions.push_back(call); block.terminator.kind = realscript::bytecode::TerminatorKind::ReturnValue; block.terminator.value = 0;
    function.blocks.push_back(block); module.functions.push_back(function);
    realscript::runtime::Interpreter interpreter({module});
    const auto result = interpreter.invoke(realscript::semantic::SymbolId{2});
    require(!result.succeeded, "unresolved external must fail");
    require(result.error.code == realscript::runtime::ErrorCode::ExternalFunctionUnresolved, "wrong external error");
}

void testDeterministicExecutionCount() {
    auto modules = compile({{"det.rs", "module Demo; int main() { int x = 3; while (x > 0) x = x - 1; return x; }"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto first = interpreter.invoke("Demo::main");
    const auto second = interpreter.invoke("Demo::main");
    require(first.succeeded && second.succeeded, "deterministic executions failed");
    require(first.value == second.value, "deterministic values differ");
    require(first.instructionsExecuted == second.instructionsExecuted, "instruction counts differ");
}
}

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
    };
    run("Arithmetic and calls", testArithmeticAndCalls);
    run("Control flow block arguments", testControlFlowAndBlockArguments);
    run("Loop execution", testLoop);
    run("Division trap", testDivisionTrap);
    run("Instruction budget", testInstructionBudget);
    run("Recursion limit", testRecursionLimit);
    run("Cross-module call", testCrossModuleCall);
    run("External resolver", testExternalResolver);
    run("Integer overflow", testIntegerOverflow);
    run("Unresolved external", testUnresolvedExternal);
    run("Deterministic execution count", testDeterministicExecutionCount);
    return failures == 0 ? 0 : 1;
}
