#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def write(path: str, text: str) -> None:
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    text = read(path)
    if new in text:
        return
    if old not in text:
        raise RuntimeError(f"anchor not found in {path}: {old[:120]!r}")
    write(path, text.replace(old, new, 1))


# Build result metadata.
replace_once(
    "include/realscript/compiler/Compilation.h",
    "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n    std::vector<LanguageSequenceRecord> nativeSequences;",
    "    std::vector<LanguageInterfaceImplementation> nativeInterfaces;\n    std::vector<LanguageGenericInstantiation> nativeGenericInstantiations;\n    std::vector<LanguageSequenceRecord> nativeSequences;")

# Integrate the compiler-owned specializer after internal work structures exist.
replace_once(
    "src/compiler/Compilation.cpp",
    "struct ModuleWork {\n    std::string name;",
    "struct ModuleWork {\n    std::string name;")
replace_once(
    "src/compiler/Compilation.cpp",
    "};\n\nstd::uint64_t combineFingerprint(\n    std::uint64_t seed,",
    "};\n\n#include \"NativeGenerics.inl\"\n\nstd::uint64_t combineFingerprint(\n    std::uint64_t seed,")
replace_once(
    "src/compiler/Compilation.cpp",
    "        units.push_back(std::move(unit));\n    }\n\n    // Stable source order and named-type shells.",
    "        units.push_back(std::move(unit));\n    }\n\n    specializeNativeGenerics(units, modules, result);\n\n    // Stable source order and named-type shells.")

# Generic templates stay in original AST/tooling but do not enter concrete binding.
replace_once(
    "src/compiler/Compilation.cpp",
    "            for (const auto& node : unit->syntaxTree->classes) {\n                const auto before = module.types.size();",
    "            for (const auto& node : unit->syntaxTree->classes) {\n                if (!node.typeParameters.empty()) continue;\n                const auto before = module.types.size();")
replace_once(
    "src/compiler/Compilation.cpp",
    "            for (const auto& node : unit->syntaxTree->structs) {\n                const auto before = module.types.size();",
    "            for (const auto& node : unit->syntaxTree->structs) {\n                if (!node.typeParameters.empty()) continue;\n                const auto before = module.types.size();")
replace_once(
    "src/compiler/Compilation.cpp",
    "            for (const auto& node : unit->syntaxTree->classes) {\n                auto* type = findOwnType(module, node.identifierToken.text);",
    "            for (const auto& node : unit->syntaxTree->classes) {\n                if (!node.typeParameters.empty()) continue;\n                auto* type = findOwnType(module, node.identifierToken.text);")
replace_once(
    "src/compiler/Compilation.cpp",
    "            for (const auto& node : unit->syntaxTree->structs) {\n                auto* type = findOwnType(module, node.identifierToken.text);",
    "            for (const auto& node : unit->syntaxTree->structs) {\n                if (!node.typeParameters.empty()) continue;\n                auto* type = findOwnType(module, node.identifierToken.text);")
replace_once(
    "src/compiler/Compilation.cpp",
    "            for (const auto& functionSyntax : unit->syntaxTree->functions) {\n                semantic::FunctionBindingInput binding;",
    "            for (const auto& functionSyntax : unit->syntaxTree->functions) {\n                if (!functionSyntax.typeParameters.empty()) continue;\n                semantic::FunctionBindingInput binding;")
replace_once(
    "src/compiler/Compilation.cpp",
    "                for (const auto& typeSyntax : declarations) {\n                    auto* ownerPointer = findOwnType(module, typeSyntax.identifierToken.text);",
    "                for (const auto& typeSyntax : declarations) {\n                    if (!typeSyntax.typeParameters.empty()) continue;\n                    auto* ownerPointer = findOwnType(module, typeSyntax.identifierToken.text);")

# Old source expansion is no longer involved.
replace_once(
    "include/realscript/compiler/LanguageExpansion.h",
    "    bool generics = true;",
    "    bool generics = false;")

# Retain native generic metadata in Game SDK products.
replace_once(
    "src/game/GameApi.cpp",
    "    auto build = compilation.build();\n    result.languageMetadata.attributes.insert(",
    "    auto build = compilation.build();\n    result.languageMetadata.genericInstantiations.insert(\n        result.languageMetadata.genericInstantiations.end(),\n        build.nativeGenericInstantiations.begin(),\n        build.nativeGenericInstantiations.end());\n    std::sort(\n        result.languageMetadata.genericInstantiations.begin(),\n        result.languageMetadata.genericInstantiations.end(),\n        [](const auto& left, const auto& right) {\n            if (left.generatedName != right.generatedName) {\n                return left.generatedName < right.generatedName;\n            }\n            if (left.genericName != right.genericName) {\n                return left.genericName < right.genericName;\n            }\n            return left.arguments < right.arguments;\n        });\n    result.languageMetadata.genericInstantiations.erase(\n        std::unique(\n            result.languageMetadata.genericInstantiations.begin(),\n            result.languageMetadata.genericInstantiations.end(),\n            [](const auto& left, const auto& right) {\n                return left.generatedName == right.generatedName &&\n                    left.genericName == right.genericName &&\n                    left.arguments == right.arguments;\n            }),\n        result.languageMetadata.genericInstantiations.end());\n    result.languageMetadata.attributes.insert(")

# Native execution/bypass/metadata coverage. Existing Phase 11-17 tests provide
# broad user type/function/builtin/cross-module/AOT regression coverage.
test_path = ROOT / "tests/phase18_native_control_flow_tests.cpp"
tests = test_path.read_text(encoding="utf-8")
anchor = "void testNativeSequenceDiagnostics() {"
insert = '''void testNativeGenericSpecialization() {
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
        "native generic specialization failed:\\n" +
            diagnosticsText(build.diagnostics));
    require(build.nativeGenericInstantiations.size() >= 3,
        "native generic specialization metadata was not retained");

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        auto module = lowerer.lower(sourceModule);
        realscript::diagnostics::DiagnosticBag diagnostics;
        require(realscript::bytecode::verifyModule(module, diagnostics),
            "native generic bytecode verification failed:\\n" +
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

void testGenericsBypassExpansion() {
    const auto expansion =
        realscript::compiler::expandLanguageSource(
            "generics.rs",
            "module Native; T Id<T>(T v){return v;} "
            "int main(){return Id<int>(3);}");
    require(!expansion.changed,
        "native generics still used source expansion");
}

'''
if insert not in tests:
    if anchor not in tests:
        raise RuntimeError("generic semantic test insertion anchor missing")
    tests = tests.replace(anchor, insert + anchor, 1)
run_anchor = '    run("native sequence diagnostics", testNativeSequenceDiagnostics);'
run_insert = '''    run("native generic specialization", testNativeGenericSpecialization);
    run("generics bypass expansion", testGenericsBypassExpansion);
'''
if run_insert not in tests:
    if run_anchor not in tests:
        raise RuntimeError("generic semantic test registration anchor missing")
    tests = tests.replace(run_anchor, run_insert + run_anchor, 1)
test_path.write_text(tests, encoding="utf-8")

# Roadmap status.
roadmap_path = ROOT / "docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md"
roadmap = roadmap_path.read_text(encoding="utf-8")
old = '''### 18D — generics — pending

- Native type-parameter/type-argument syntax and symbols.
- Generic construction and deterministic specialization after semantic binding.
- Stable generic identity independent of generated source names.'''
new = '''### 18D — generics — complete for explicit bounded specialization

Implemented and validated:

- native type-parameter and nested type-argument syntax;
- native explicit generic type/function call syntax;
- compiler-owned specialization units without rewriting user source;
- deterministic concrete names and stable specialization metadata;
- user generic classes, structs, and free functions;
- fixed-capacity `List`, `Queue`, `Stack`, `Optional`, `HashSet`, and `Dictionary` profiles;
- cross-file/module explicit specialization and AOT/JIT reuse through normal MIR;
- `LanguageExpansionOptions::generics` disabled by default.

Inference, constraints, generic member methods, generic interfaces/delegates, and complete collection implementations remain Phase 21 work.'''
if old in roadmap:
    roadmap = roadmap.replace(old, new, 1)
elif new not in roadmap:
    # tolerate roadmap title without pending suffix
    old2 = old.replace(" — pending", "")
    if old2 not in roadmap:
        raise RuntimeError("generic roadmap anchor missing")
    roadmap = roadmap.replace(old2, new, 1)
roadmap_path.write_text(roadmap, encoding="utf-8")

print("native Phase 18 generic semantics integrated")
