#!/usr/bin/env python3
from pathlib import Path
import re

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


def remove_test_functions_with(path: str, needle: str) -> list[str]:
    text = read(path)
    removed: list[str] = []
    pattern = re.compile(
        r"\nvoid (test[A-Za-z0-9_]+)\(\) \{.*?\n\}\n(?=\nvoid |\n\} // namespace)",
        re.S)
    while True:
        match = None
        for candidate in pattern.finditer(text):
            if needle in candidate.group(0):
                match = candidate
                break
        if match is None:
            break
        removed.append(match.group(1))
        text = text[:match.start()] + "\n" + text[match.end():]
    for name in removed:
        text = re.sub(
            rf"^\s*run\([^\n]*\b{name}\b[^\n]*\);\n",
            "",
            text,
            flags=re.M)
    write(path, text)
    return removed


# Metadata is part of the compiler product, independent of the retired source
# expansion implementation.
metadata = '''#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace realscript::compiler {

struct LanguageAttributeArgument {
    std::string name;
    std::string value;
};

struct LanguageAttributeRecord {
    std::string target;
    std::string name;
    std::vector<LanguageAttributeArgument> arguments;
    std::string sourceName;
    std::size_t offset = 0;
};

struct LanguageInterfaceImplementation {
    std::string typeName;
    std::vector<std::string> interfaces;
};

struct LanguageGenericInstantiation {
    std::string genericName;
    std::vector<std::string> arguments;
    std::string generatedName;
};

struct LanguageSequenceRecord {
    std::string typeName;
    std::string name;
    std::vector<std::string> callbacks;
    std::string sourceName;
    std::size_t offset = 0;
};

} // namespace realscript::compiler
'''
write("include/realscript/compiler/LanguageMetadata.h", metadata)

# Compilation owns original source only. There is no compatibility preprocessing
# stage and therefore no expansion options/results in the public API.
compilation_header = read("include/realscript/compiler/Compilation.h")
compilation_header = compilation_header.replace(
    '#include "realscript/compiler/LanguageExpansion.h"',
    '#include "realscript/compiler/LanguageMetadata.h"')
class_pattern = re.compile(
    r"class Compilation \{\npublic:.*?\n\};\n\n\[\[nodiscard\]\] std::uint64_t stableFingerprint",
    re.S)
class_replacement = '''class Compilation {
public:
    Compilation() = default;
    explicit Compilation(std::vector<SourceFile> sources)
        : sources_(std::move(sources)) {}

    void addSource(SourceFile source) {
        sources_.push_back(std::move(source));
    }

    [[nodiscard]] BuildResult build(
        const BuildSnapshot* previous = nullptr) const;

private:
    std::vector<SourceFile> sources_;
};

[[nodiscard]] std::uint64_t stableFingerprint'''
compilation_header, count = class_pattern.subn(
    class_replacement, compilation_header, count=1)
if count != 1:
    raise RuntimeError("Compilation public API block not found")
write("include/realscript/compiler/Compilation.h", compilation_header)

# Game SDK consumes native build metadata only.
game = read("src/game/GameApi.cpp")
helper_pattern = re.compile(
    r"\nvoid collectLanguageMetadata\(.*?\n\}\n\n\} // namespace",
    re.S)
game, count = helper_pattern.subn("\n\n} // namespace", game, count=1)
if count != 1:
    raise RuntimeError("legacy GameApi metadata collector not found")
game = game.replace(
    '''    collectLanguageMetadata(
        result.languageMetadata,
        compilation.languageExpansions());

''',
    "")
write("src/game/GameApi.cpp", game)

# Build graph and test naming reflect the native compiler pipeline.
cmake = read("CMakeLists.txt")
cmake = cmake.replace("    src/compiler/LanguageExpansion.cpp\n", "")
cmake = cmake.replace(
    '''        realscript_phase11_17_language_expansion_tests
        realscript.phase11-17.language-expansion
        tests/phase11_17_language_expansion_tests.cpp)''',
    '''        realscript_phase11_18_native_language_tests
        realscript.phase11-18.native-language
        tests/phase11_18_native_language_tests.cpp)''')
write("CMakeLists.txt", cmake)

# Retain native language/AOT/metadata/sequence regression coverage while removing
# tests of a public API that no longer exists.
old_test = ROOT / "tests/phase11_17_language_expansion_tests.cpp"
new_test = ROOT / "tests/phase11_18_native_language_tests.cpp"
text = old_test.read_text(encoding="utf-8")
# Remove the standalone options/refresh test.
text = re.sub(
    r"\nvoid testExpansionOptionsRefreshExistingSources\(\) \{.*?\n\}\n(?=\nvoid )",
    "\n",
    text,
    count=1,
    flags=re.S)
text = re.sub(
    r'^\s*run\("expansion options refresh",\s*testExpansionOptionsRefreshExistingSources\);\n',
    "",
    text,
    flags=re.M)
# Sequence test remains; only the retired bypass assertion is removed.
text = re.sub(
    r'''\n    const auto expansion = realscript::compiler::expandLanguageSource\(
        "native-sequence\.rs", source\);
    require\(!expansion\.changed,
        "sequence source still used expansion"\);
''',
    "\n",
    text,
    count=1)
text = text.replace("testExpansionMetadata", "testNativeMetadata")
text = text.replace("testAotGenerationFromExpandedSource", "testNativeAotGeneration")
text = text.replace("source attribute metadata", "native source metadata")
text = text.replace("AOT generation from expanded source", "AOT generation from native source")
text = text.replace("expanded AOT source", "native AOT source")
text = text.replace("expanded MIR", "native MIR")
text = text.replace("extended source compilation", "native source compilation")
new_test.write_text(text, encoding="utf-8")
old_test.unlink()

# Remove small dedicated bypass tests from the Phase 18 suite. Their replacement
# is structural: the expansion API and implementation no longer exist.
removed = remove_test_functions_with(
    "tests/phase18_native_control_flow_tests.cpp",
    "expandLanguageSource")
if not removed:
    raise RuntimeError("no Phase 18 expansion bypass tests were removed")

# Delete the retired implementation as one repository operation.
for path in sorted((ROOT / "src/compiler").glob("LanguageExpansion*")):
    if path.is_file():
        path.unlink()
old_header = ROOT / "include/realscript/compiler/LanguageExpansion.h"
if old_header.exists():
    old_header.unlink()

# Product docs now describe one native pipeline rather than a migration layer.
roadmap_path = ROOT / "docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md"
roadmap = roadmap_path.read_text(encoding="utf-8")
roadmap = roadmap.replace(
    "## Phase 18 — native Phase 11–17 compiler features",
    "## Phase 18 — native Phase 11–17 compiler features — complete")
roadmap = roadmap.replace(
    '''### Phase 18 exit criteria

- `LanguageExpansionOptions` is no longer required for Phase 11–17 source.
- `LanguageExpansion.cpp` and generated support declarations can be removed.
- Interpreter/AOT/JIT result, digest, and profile differential tests pass.
- LSP/DAP/hot reload operate on original source constructs.''',
    '''### Phase 18 exit criteria — complete

- `Compilation` consumes original source directly;
- `LanguageExpansionOptions`, `LanguageExpansionResult`, and the expansion API are removed;
- `LanguageExpansion.cpp` and all expansion `.inl` files are deleted;
- native attributes, interfaces, generic instantiations, and sequences flow through `LanguageMetadata.h`;
- Interpreter/AOT/JIT share the same native Syntax/Bound/MIR pipeline;
- LSP/DAP/hot reload operate on original source constructs and stable compiler-owned synthetic symbols.''')
roadmap_path.write_text(roadmap, encoding="utf-8")

profile_path = ROOT / "docs/en/LANGUAGE_EXPANSION_PHASE_11_17.md"
if profile_path.exists():
    profile_path.rename(ROOT / "docs/en/NATIVE_LANGUAGE_PHASE_11_18.md")
    profile = (ROOT / "docs/en/NATIVE_LANGUAGE_PHASE_11_18.md").read_text(encoding="utf-8")
    profile = profile.replace("# Phase 11–18 Language Profile", "# Phase 11–18 Native Language Profile")
    profile = re.sub(
        r"RealScript originally introduced.*?This remains an embedded deterministic game-language profile, not CLR compatibility\.\n",
        "RealScript Phase 18 compiles the C#-style gameplay profile directly through the native lexer, syntax tree, Binder, flow analysis, Typed MIR, bytecode, AOT/JIT pipeline, tooling metadata, and Game SDK.\n\nThis remains an embedded deterministic game-language profile, not CLR compatibility.\n",
        profile,
        count=1,
        flags=re.S)
    profile = profile.replace("## Current compilation model", "## Compilation model")
    profile = re.sub(
        r"A `Compilation` still runs.*?Declarations remain isolated by `module`; directly imported modules contribute visible interface and remaining expansion declarations\.\n",
        "A `Compilation` parses original source directly. There is no source expansion or compatibility preprocessing stage. Declarations remain isolated by `module`; directly imported modules contribute visible native declarations and contracts.\n",
        profile,
        count=1,
        flags=re.S)
    profile = profile.replace("Currently available through the compatibility expansion path:", "Implemented directly in the compiler:")
    profile = profile.replace("Currently available through compatibility specialization:", "Implemented through compiler-owned explicit specialization:")
    profile = profile.replace("Currently available through compatibility lowering:", "Implemented through native sequence nodes and compiler-owned callbacks:")
    profile = profile.replace("Currently available through compatibility lowering:", "Implemented directly in the compiler:")
    profile = re.sub(
        r"## Remaining Phase 18 work\n.*?The migration remains explicit so game code depends only on verified implemented behavior\.\n",
        "## Phase 18 closure\n\nThe source-expansion implementation and API have been removed. All Phase 11–17 profile features compile through original source nodes and the shared native backend pipeline. Broader runtime polymorphism, first-class closures, complete generics, complete coroutine state machines, and exact value/reference semantics continue in Phases 19–23.\n",
        profile,
        count=1,
        flags=re.S)
    (ROOT / "docs/en/NATIVE_LANGUAGE_PHASE_11_18.md").write_text(profile, encoding="utf-8")

# Update documentation index references where present.
for path in [ROOT / "docs/en/README.md", ROOT / "docs/README.md", ROOT / "README.md"]:
    if path.exists():
        value = path.read_text(encoding="utf-8")
        value = value.replace(
            "LANGUAGE_EXPANSION_PHASE_11_17.md",
            "NATIVE_LANGUAGE_PHASE_11_18.md")
        value = value.replace(
            "Phase 11–17 Language Expansion",
            "Phase 11–18 Native Language")
        path.write_text(value, encoding="utf-8")

# Reject forgotten compile-time dependencies on the retired API.
leftovers: list[str] = []
for base in ["include", "src", "tests", "tools"]:
    for path in (ROOT / base).rglob("*"):
        if not path.is_file() or path.suffix not in {".h", ".hpp", ".cpp", ".c", ".py"}:
            continue
        if path.name == Path(__file__).name:
            continue
        value = path.read_text(encoding="utf-8", errors="ignore")
        for token in [
            "LanguageExpansion",
            "expandLanguageSource",
            "expandLanguageSources",
            "languageExpansions()",
            "setLanguageExpansionOptions"]:
            if token in value:
                leftovers.append(f"{path.relative_to(ROOT)}: {token}")
if leftovers:
    raise RuntimeError("retired expansion references remain:\n" + "\n".join(leftovers))

print("Phase 18 source-expansion implementation removed")
