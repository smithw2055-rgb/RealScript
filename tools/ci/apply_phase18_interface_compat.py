#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]


def patch(path: str, old: str, new: str) -> None:
    file = root / path
    text = file.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"anchor missing in {path}")
    file.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


patch(
    "src/compiler/LanguageExpansionStatementsPart04.inl",
    '''        output.push_back(tokens[index++]);
        output.push_back(tokens[index++]);
        if (!symbol(tokens[index], "{")) continue;
        const auto open = index;
''',
    '''        output.push_back(tokens[index++]);
        output.push_back(tokens[index++]);
        while (index < tokens.size() && !symbol(tokens[index], "{")) {
            output.push_back(tokens[index++]);
        }
        if (index >= tokens.size() || !symbol(tokens[index], "{")) {
            continue;
        }
        const auto open = index;
''')

patch(
    "src/compiler/Compilation.cpp",
    '''        validateInterfaces(
            module.units.empty()
                ? std::vector<syntax::ClassDeclarationSyntax>{}
                : std::vector<syntax::ClassDeclarationSyntax>{});
        for (const auto* unit : module.units) {
''',
    '''        for (const auto* unit : module.units) {
''')

print("Native interface compatibility fixes applied")
