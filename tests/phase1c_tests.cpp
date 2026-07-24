#include "realscript/compiler/Compilation.h"
#include "realscript/mir/Mir.h"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool containsDiagnostic(
    const realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& code) {
    for (const auto& diagnostic : diagnostics.items()) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

realscript::compiler::BuildResult build(
    std::initializer_list<realscript::compiler::SourceFile> files,
    const realscript::compiler::BuildSnapshot* previous = nullptr) {
    realscript::compiler::Compilation compilation;
    for (auto file : files) {
        compilation.addSource(std::move(file));
    }
    return compilation.build(previous);
}

void testDirectAndForwardCall() {
    const auto result = build({{
        "main.rs",
        "module App; "
        "int main() { return add(2, 3); } "
        "int add(int a, int b) { return a + b; }",
    }});

    require(!result.diagnostics.hasErrors(), "forward call failed");
    const auto mir = realscript::mir::printModule(result.modules.front());
    require(
        mir.find("call @App::add") != std::string::npos,
        "call MIR missing");
}

void testOverloadExactMatch() {
    const auto result = build({{
        "main.rs",
        "module App; "
        "int pick(int value) { return value; } "
        "string pick(string value) { return value; } "
        "int main() { return pick(7); }",
    }});

    require(!result.diagnostics.hasErrors(), "exact overload failed");
    require(
        result.modules.front().functions.size() == 3,
        "overloads were not retained");
}

void testNullConversionChoosesString() {
    const auto result = build({{
        "main.rs",
        "module App; "
        "string pick(string value) { return value; } "
        "string main() { return pick(null); }",
    }});

    require(!result.diagnostics.hasErrors(), "null conversion failed");
    const auto mir = realscript::mir::printModule(result.modules.front());
    require(
        mir.find("conv.null.string") != std::string::npos,
        "conversion MIR missing");
}

void testNoApplicableOverload() {
    const auto result = build({{
        "main.rs",
        "module App; "
        "int pick(int value) { return value; } "
        "int main() { return pick(\"text\"); }",
    }});

    require(
        containsDiagnostic(result.diagnostics, "RS2107"),
        "missing no-applicable-overload diagnostic");
}

void testImportedFunction() {
    const auto result = build({
        {
            "lib.rs",
            "module Lib; int twice(int value) { return value * 2; }",
        },
        {
            "app.rs",
            "module App; import Lib; int main() { return twice(4); }",
        },
    });

    require(!result.diagnostics.hasErrors(), "imported call failed");
    require(result.modules.size() == 2, "expected two modules");
}

void testMissingImport() {
    const auto result = build({{
        "app.rs",
        "module App; import Missing; int main() { return 0; }",
    }});

    require(
        containsDiagnostic(result.diagnostics, "RS4001"),
        "missing-import diagnostic absent");
}

void testAmbiguousImports() {
    const auto result = build({
        {
            "a.rs",
            "module A; int choose(int value) { return value; }",
        },
        {
            "b.rs",
            "module B; int choose(int value) { return value; }",
        },
        {
            "app.rs",
            "module App; import A; import B; "
            "int main() { return choose(1); }",
        },
    });

    require(
        containsDiagnostic(result.diagnostics, "RS2108"),
        "ambiguous-call diagnostic absent");
}

void testStableIdsIgnoreSourceOrder() {
    const auto first = build({{
        "one.rs",
        "module App; "
        "int a() { return 1; } "
        "int b(int value) { return value; }",
    }});
    const auto second = build({{
        "one.rs",
        "module App; "
        "int b(int value) { return value; } "
        "int a() { return 1; }",
    }});

    require(
        !first.diagnostics.hasErrors() && !second.diagnostics.hasErrors(),
        "stable-id build failed");

    std::uint64_t firstId = 0;
    std::uint64_t secondId = 0;
    for (const auto& function : first.modules.front().functions) {
        if (function.name == "b") {
            firstId = function.symbolId;
        }
    }
    for (const auto& function : second.modules.front().functions) {
        if (function.name == "b") {
            secondId = function.symbolId;
        }
    }
    require(
        firstId != 0 && firstId == secondId,
        "stable SymbolId changed with source order");
}

void testMultiFileSameModule() {
    const auto result = build({
        {
            "a.rs",
            "module App; int helper() { return 4; }",
        },
        {
            "b.rs",
            "module App; int main() { return helper(); }",
        },
    });

    require(!result.diagnostics.hasErrors(), "partial module call failed");
    require(
        result.modules.size() == 1 &&
            result.modules.front().functions.size() == 2,
        "partial module aggregation failed");
}

void testIncrementalReuseAndInvalidation() {
    const auto first = build({
        {
            "lib.rs",
            "module Lib; int value() { return 1; }",
        },
        {
            "app.rs",
            "module App; import Lib; int main() { return value(); }",
        },
    });
    require(
        !first.diagnostics.hasErrors(),
        "initial incremental build failed");

    const auto second = build({
        {
            "lib.rs",
            "module Lib; int value() { return 2; }",
        },
        {
            "app.rs",
            "module App; import Lib; int main() { return value(); }",
        },
    }, &first.snapshot);
    require(
        !second.diagnostics.hasErrors(),
        "implementation-only edit build failed");

    bool appReused = false;
    bool libraryReused = true;
    for (const auto& module : second.buildInfo) {
        if (module.name == "App") {
            appReused = module.reused;
        }
        if (module.name == "Lib") {
            libraryReused = module.reused;
        }
    }
    require(
        appReused && !libraryReused,
        "implementation-only edit invalidated the wrong modules");

    const auto third = build({
        {
            "lib.rs",
            "module Lib; "
            "int value() { return 2; } "
            "int extra() { return 3; }",
        },
        {
            "app.rs",
            "module App; import Lib; int main() { return value(); }",
        },
    }, &second.snapshot);

    bool dependentReused = true;
    for (const auto& module : third.buildInfo) {
        if (module.name == "App") {
            dependentReused = module.reused;
        }
    }
    require(
        !dependentReused,
        "public API edit did not invalidate the dependent module");
}

void testVoidCallExpressionStatement() {
    const auto result = build({{
        "main.rs",
        "module App; "
        "void ping() { return; } "
        "int main() { ping(); return 0; }",
    }});

    require(!result.diagnostics.hasErrors(), "void call statement failed");
    const auto mir = realscript::mir::printModule(result.modules.front());
    require(
        mir.find("call @App::ping") != std::string::npos,
        "void call MIR missing");
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

    run("Direct and forward call", testDirectAndForwardCall);
    run("Overload exact match", testOverloadExactMatch);
    run("Null conversion", testNullConversionChoosesString);
    run("No applicable overload", testNoApplicableOverload);
    run("Imported function", testImportedFunction);
    run("Missing import", testMissingImport);
    run("Ambiguous imports", testAmbiguousImports);
    run("Stable SymbolId", testStableIdsIgnoreSourceOrder);
    run("Multi-file module", testMultiFileSameModule);
    run("Incremental reuse", testIncrementalReuseAndInvalidation);
    run("Void call statement", testVoidCallExpressionStatement);

    return failures == 0 ? 0 : 1;
}
