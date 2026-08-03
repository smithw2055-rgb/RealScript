#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]

symbols_path = root / "src/semantic/Symbols.cpp"
symbols = symbols_path.read_text(encoding="utf-8")
old = '''PrimitiveType resolvePrimitiveType(const std::string& name) noexcept {
    if (name == "void") return PrimitiveType::Void;
    if (name == "bool") return PrimitiveType::Bool;
    if (name == "int") return PrimitiveType::Int;
    if (name == "long") return PrimitiveType::Long;
    if (name == "double") return PrimitiveType::Double;
    if (name == "string") return PrimitiveType::String;
    if (name == "handle") return PrimitiveType::Handle;
    return PrimitiveType::Error;
}
'''
new = '''PrimitiveType resolvePrimitiveType(const std::string& name) noexcept {
    if (name == "void") return PrimitiveType::Void;
    if (name == "bool") return PrimitiveType::Bool;
    if (name == "int" || name == "byte" || name == "sbyte" ||
        name == "short" || name == "ushort" || name == "char") {
        return PrimitiveType::Int;
    }
    if (name == "long" || name == "uint" || name == "ulong") {
        return PrimitiveType::Long;
    }
    if (name == "double" || name == "float") {
        return PrimitiveType::Double;
    }
    if (name == "string") return PrimitiveType::String;
    if (name == "handle") return PrimitiveType::Handle;
    return PrimitiveType::Error;
}
'''
if new not in symbols:
    if old not in symbols:
        raise RuntimeError("primitive type resolver anchor not found")
    symbols = symbols.replace(old, new, 1)
symbols_path.write_text(symbols, encoding="utf-8")

expansion_path = root / "include/realscript/compiler/LanguageExpansion.h"
expansion = expansion_path.read_text(encoding="utf-8")
expansion = expansion.replace(
    "    bool valueTypeAliases = true;",
    "    bool valueTypeAliases = false;")
expansion_path.write_text(expansion, encoding="utf-8")

tests_path = root / "tests/phase18_native_control_flow_tests.cpp"
tests = tests_path.read_text(encoding="utf-8")
anchor = "void testNativeSequenceDiagnostics() {"
insert = '''void testNativeValueAliases() {
    const auto result = execute(R"(
module Phase18;
int main()
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

'''
if insert not in tests:
    if anchor not in tests:
        raise RuntimeError("phase18 test insertion anchor not found")
    tests = tests.replace(anchor, insert + anchor, 1)
run_anchor = '    run("native sequence diagnostics", testNativeSequenceDiagnostics);'
run_insert = '''    run("native value aliases", testNativeValueAliases);
    run("aliases bypass expansion", testAliasesBypassExpansion);
'''
if run_insert not in tests:
    if run_anchor not in tests:
        raise RuntimeError("phase18 test registration anchor not found")
    tests = tests.replace(run_anchor, run_insert + run_anchor, 1)
tests_path.write_text(tests, encoding="utf-8")

roadmap_path = root / "docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md"
roadmap = roadmap_path.read_text(encoding="utf-8")
roadmap = roadmap.replace(
    "Still pending in 18F:\n\n- reference member/indexer l-values and complete alias analysis;\n- native exact-width aliases;\n- complete reference semantics, nullable values, and boxing in Phase 23.",
    "The bounded Phase 18F profile is complete:\n\n- `byte`, `sbyte`, `short`, `ushort`, and `char` resolve natively to the checked `int` carrier;\n- `uint` and `ulong` resolve natively to the checked `long` carrier;\n- `float` resolves natively to the `double` carrier;\n- `LanguageExpansionOptions::valueTypeAliases` is disabled by default;\n- aliases preserve original source spans and no longer generate source text.\n\nReference member/indexer l-values, exact-width identities, checked/unchecked conversions, nullable values, and boxing remain Phase 23 work.")
roadmap = roadmap.replace(
    "### 18F — reference modifiers and exact aliases — in progress",
    "### 18F — reference modifiers and value aliases — complete")
roadmap_path.write_text(roadmap, encoding="utf-8")

print("native Phase 18 aliases applied")
