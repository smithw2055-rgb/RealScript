#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

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

std::vector<realscript::mir::Module> compileMir(const std::string& source) {
    realscript::compiler::Compilation compilation;
    compilation.addSource({"phase6.rs", source});
    auto result = compilation.build();
    require(!result.diagnostics.hasErrors(), "Phase 6 source compilation failed");
    return result.modules;
}

std::vector<realscript::bytecode::Module> lower(
    const std::vector<realscript::mir::Module>& modules) {
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> result;
    for (const auto& module : modules) result.push_back(lowerer.lower(module));
    return result;
}

std::size_t instructionCount(const std::vector<realscript::mir::Module>& modules) {
    std::size_t count = 0;
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            for (const auto& block : function.blocks) {
                count += block.instructions.size();
            }
        }
    }
    return count;
}

std::size_t blockCount(const std::vector<realscript::mir::Module>& modules) {
    std::size_t count = 0;
    for (const auto& module : modules) {
        for (const auto& function : module.functions) count += function.blocks.size();
    }
    return count;
}

realscript::bytecode::Module externalModule(std::int64_t argument) {
    realscript::bytecode::Module module;
    module.name = "Phase6.External";
    realscript::bytecode::FunctionReference reference;
    reference.symbolId = 0x6001;
    reference.name = "Host::sample";
    reference.returnType = realscript::semantic::PrimitiveType::Int;
    reference.parameterTypes = {realscript::semantic::PrimitiveType::Int};
    reference.parameterTypeIds = {0};
    module.functionReferences.push_back(reference);

    realscript::bytecode::Function function;
    function.symbolId = 0x6002;
    function.name = "main";
    function.returnType = realscript::semantic::PrimitiveType::Int;
    function.registerTypes = {
        realscript::semantic::PrimitiveType::Int,
        realscript::semantic::PrimitiveType::Int,
    };
    function.registerTypeIds = {0, 0};
    realscript::bytecode::BasicBlock block;
    block.id = 0;
    realscript::bytecode::Instruction constant;
    constant.opcode = realscript::bytecode::Opcode::ConstantInt;
    constant.result = 0;
    constant.integerImmediate = argument;
    block.instructions.push_back(std::move(constant));
    realscript::bytecode::Instruction call;
    call.opcode = realscript::bytecode::Opcode::Call;
    call.result = 1;
    call.index = 0;
    call.operands = {0};
    block.instructions.push_back(std::move(call));
    block.terminator.kind = realscript::bytecode::TerminatorKind::ReturnValue;
    block.terminator.value = 1;
    function.blocks.push_back(block);
    module.functions.push_back(function);
    return module;
}

void testMirOptimizer() {
    const auto source = R"(
module Phase6.Optimize;

int folded()
{
    int dead = 1 + 2;
    if (true)
        return (10 + 20) * 2;
    else
        return 99;
}

int overflow()
{
    return 2147483647 + 1;
}
)";
    auto original = compileMir(source);
    const auto originalInstructions = instructionCount(original);
    const auto originalBlocks = blockCount(original);

    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::optimization::Optimizer optimizer;
    realscript::optimization::Options options;
    options.level = realscript::optimization::Level::Aggressive;
    auto optimized = optimizer.optimize(original, diagnostics, options);
    require(!diagnostics.hasErrors(), "optimizer emitted diagnostics");
    require(optimized.statistics.constantsFolded >= 3,
        "optimizer did not fold constant expressions");
    require(optimized.statistics.branchesFolded >= 1,
        "optimizer did not fold the constant branch");
    require(optimized.statistics.blocksRemoved >= 1,
        "optimizer did not remove unreachable blocks");
    require(instructionCount(optimized.modules) < originalInstructions,
        "optimizer did not reduce the instruction count");
    require(blockCount(optimized.modules) < originalBlocks,
        "optimizer did not reduce the block count");

    auto originalBytecode = lower(original);
    auto optimizedBytecode = lower(optimized.modules);
    realscript::diagnostics::DiagnosticBag bytecodeDiagnostics;
    for (const auto& module : optimizedBytecode) {
        (void)realscript::bytecode::verifyModule(module, bytecodeDiagnostics);
    }
    if (bytecodeDiagnostics.hasErrors()) {
        for (const auto& diagnostic : bytecodeDiagnostics.items()) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    realscript::runtime::Interpreter originalRuntime(std::move(originalBytecode));
    realscript::runtime::Interpreter optimizedRuntime(std::move(optimizedBytecode));
    const auto originalValue = originalRuntime.invoke("Phase6.Optimize::folded");
    const auto optimizedValue = optimizedRuntime.invoke("Phase6.Optimize::folded");
    if (!(originalValue.succeeded && optimizedValue.succeeded &&
            std::get<std::int64_t>(originalValue.value) == 60 &&
            originalValue.value == optimizedValue.value)) {
        std::cerr << "ORIGINAL MIR\n" << realscript::mir::printModule(original.front())
            << "OPTIMIZED MIR\n" << realscript::mir::printModule(optimized.modules.front())
            << "original success=" << originalValue.succeeded
            << " value=" << realscript::runtime::valueToString(originalValue.value)
            << " error=" << originalValue.error.message << '\n'
            << "optimized success=" << optimizedValue.succeeded
            << " value=" << realscript::runtime::valueToString(optimizedValue.value)
            << " error=" << optimizedValue.error.message << '\n';
        throw std::runtime_error("optimized MIR changed the successful result");
    }

    const auto originalOverflow = originalRuntime.invoke("Phase6.Optimize::overflow");
    const auto optimizedOverflow = optimizedRuntime.invoke("Phase6.Optimize::overflow");
    require(!originalOverflow.succeeded && !optimizedOverflow.succeeded &&
            originalOverflow.error.code ==
                realscript::runtime::ErrorCode::IntegerOverflow &&
            optimizedOverflow.error.code == originalOverflow.error.code,
        "optimizer folded away a checked overflow trap");
}

void testDeterministicRecordReplay() {
    auto bindings = std::make_shared<realscript::runtime::BindingRegistry>();
    int calls = 0;
    require(bindings->bind(
        "Host::sample",
        [&](const auto&, const auto& arguments, auto&) {
            ++calls;
            return std::optional<realscript::runtime::Value>{
                std::get<std::int64_t>(arguments.front()) + calls};
        },
        realscript::runtime::BindingDeterminism::Recordable),
        "recordable host binding registration failed");

    realscript::runtime::Interpreter runtime({externalModule(41)});
    runtime.setBindingRegistry(bindings);

    realscript::runtime::ExecutionOptions missingLogOptions;
    missingLogOptions.determinism.mode =
        realscript::runtime::DeterminismMode::Record;
    const auto missingLog = runtime.invoke(
        realscript::semantic::SymbolId{0x6002}, {}, missingLogOptions);
    require(!missingLog.succeeded && calls == 0 &&
            missingLog.error.code ==
                realscript::runtime::ErrorCode::DeterminismViolation,
        "record mode invoked the host before validating its replay log");

    auto log = std::make_shared<realscript::runtime::ReplayLog>();
    realscript::runtime::ExecutionOptions recordOptions;
    recordOptions.determinism.mode = realscript::runtime::DeterminismMode::Record;
    recordOptions.determinism.replayLog = log;
    const auto recorded = runtime.invoke(
        realscript::semantic::SymbolId{0x6002}, {}, recordOptions);
    require(recorded.succeeded &&
            std::get<std::int64_t>(recorded.value) == 42 &&
            calls == 1 && log->size() == 1,
        "record mode did not capture the external result");

    realscript::runtime::ExecutionOptions replayOptions;
    replayOptions.determinism.mode = realscript::runtime::DeterminismMode::Replay;
    replayOptions.determinism.replayLog = log;
    // Replay must remain usable when the original non-deterministic host
    // service is unavailable in the replay process.
    runtime.setBindingRegistry(
        std::make_shared<realscript::runtime::BindingRegistry>());
    const auto replayed = runtime.invoke(
        realscript::semantic::SymbolId{0x6002}, {}, replayOptions);
    require(replayed.succeeded &&
            std::get<std::int64_t>(replayed.value) == 42 &&
            calls == 1 && replayed.replayEntriesConsumed == 1,
        "replay mode called the host or lost the recorded result");
    require(recorded.determinismDigest == replayed.determinismDigest,
        "record and replay produced different execution digests");

    auto extraLog = std::make_shared<realscript::runtime::ReplayLog>();
    const auto records = log->entries();
    require(records.size() == 1, "record log changed unexpectedly");
    extraLog->append(records.front());
    extraLog->append(records.front());
    replayOptions.determinism.replayLog = extraLog;
    const auto extraReplay = runtime.invoke(
        realscript::semantic::SymbolId{0x6002}, {}, replayOptions);
    require(!extraReplay.succeeded &&
            extraReplay.error.code ==
                realscript::runtime::ErrorCode::ReplayMismatch &&
            extraReplay.replayEntriesConsumed == 1,
        "replay accepted an unconsumed external-call entry");

    runtime.setBindingRegistry(bindings);
    realscript::runtime::ExecutionOptions strictOptions;
    strictOptions.determinism.mode = realscript::runtime::DeterminismMode::Strict;
    const auto strict = runtime.invoke(
        realscript::semantic::SymbolId{0x6002}, {}, strictOptions);
    require(!strict.succeeded &&
            strict.error.code ==
                realscript::runtime::ErrorCode::DeterminismViolation,
        "strict mode accepted a recordable external binding");

    auto mismatchLog = std::make_shared<realscript::runtime::ReplayLog>();
    realscript::runtime::ExternalCallRecord mismatch;
    mismatch.symbolId = 0x6001;
    mismatch.name = "Host::sample";
    mismatch.argumentHash = 123;
    mismatch.succeeded = true;
    mismatch.result = std::int64_t{42};
    mismatchLog->append(mismatch);
    replayOptions.determinism.replayLog = mismatchLog;
    const auto mismatchResult = runtime.invoke(
        realscript::semantic::SymbolId{0x6002}, {}, replayOptions);
    require(!mismatchResult.succeeded &&
            mismatchResult.error.code ==
                realscript::runtime::ErrorCode::ReplayMismatch,
        "replay accepted a mismatched external call");
}

void testProfilingAndDigest() {
    const auto source = R"(
module Phase6.Profile;
int helper(int value) { return value * 2; }
int main()
{
    int index = 0;
    int total = 0;
    while (index < 4)
    {
        total = total + helper(index);
        index = index + 1;
    }
    return total;
}
)";
    realscript::runtime::Interpreter runtime(lower(compileMir(source)));
    auto collector = std::make_shared<realscript::runtime::ProfileCollector>();
    realscript::runtime::ExecutionOptions options;
    options.profile = collector;
    options.determinism.mode = realscript::runtime::DeterminismMode::Strict;
    const auto first = runtime.invoke("Phase6.Profile::main", {}, options);
    const auto second = runtime.invoke("Phase6.Profile::main", {}, options);
    require(first.succeeded && second.succeeded &&
            first.determinismDigest == second.determinismDigest,
        "pure deterministic executions produced different digests");
    const auto profile = collector->snapshot();
    require(profile.totalEvents != 0 && profile.functions.size() >= 2,
        "profile collector did not observe function execution");
    bool foundMain = false;
    bool foundHelper = false;
    for (const auto& function : profile.functions) {
        if (function.function == "Phase6.Profile::main") {
            foundMain = function.calls == 2 && function.instructions != 0 &&
                function.branches != 0;
        }
        if (function.function == "Phase6.Profile::helper") {
            foundHelper = function.calls == 8 && function.instructions != 0;
        }
    }
    require(foundMain && foundHelper,
        "profile counts did not preserve function attribution");
    const auto json = realscript::runtime::profileToJson(profile);
    require(json.find("Phase6.Profile::main") != std::string::npos &&
            json.find("\"totalEvents\"") != std::string::npos,
        "profile JSON is incomplete");
}

} // namespace

int main() {
    try {
        testMirOptimizer();
        testDeterministicRecordReplay();
        testProfilingAndDigest();
        std::cout << "Phase 6 optimizer, determinism, and profiling tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
