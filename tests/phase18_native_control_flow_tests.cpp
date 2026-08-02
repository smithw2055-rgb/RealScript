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
void require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

std::string diagnosticsText(const realscript::diagnostics::DiagnosticBag& diagnostics) {
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
    require(!build.diagnostics.hasErrors(), "native source failed to compile:\n" + diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        auto module = lowerer.lower(sourceModule);
        realscript::diagnostics::DiagnosticBag diagnostics;
        const auto verified =
            realscript::bytecode::verifyModule(module, diagnostics);
        require(verified,
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
    require(result.succeeded, "native control-flow execution failed: " + result.error.message);
    require(std::get<std::int64_t>(result.value) == 32, "native control-flow result was incorrect");
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
    require(result.succeeded && std::get<std::int64_t>(result.value) == 7,
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
    require(build.diagnostics.hasErrors(), "invalid native loop control was accepted");
    bool breakFound = false, continueFound = false;
    for (const auto& diagnostic : build.diagnostics.items()) {
        breakFound = breakFound || diagnostic.code == "RS2212";
        continueFound = continueFound || diagnostic.code == "RS2213";
    }
    require(breakFound && continueFound, "native loop-control diagnostics were not preserved");
}

void testNoStructuredSourceRewrite() {
    const auto expansion = realscript::compiler::expandLanguageSource(
        "native.rs", "module Native; int main(){for(int i=0;i<1;i=i+1){}return 1;}");
    require(!expansion.changed, "native for statement still used source expansion");
}
}

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
    };
    run("native structured control flow", testNativeControlFlowExecution);
    run("native infinite loop break", testNativeInfiniteLoopBreak);
    run("native control diagnostics", testNativeDiagnostics);
    run("structured control flow bypasses expansion", testNoStructuredSourceRewrite);
    return failures == 0 ? 0 : 1;
}
