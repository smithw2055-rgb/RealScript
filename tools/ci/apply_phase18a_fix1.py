#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]

def patch(path, old, new):
    file = root / path
    text = file.read_text(encoding="utf-8")
    if old not in text:
        raise RuntimeError(f"anchor missing: {path}: {old[:60]}")
    file.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")

patch(
    "src/mir/MirLowerer.cpp",
    '''namespace realscript::mir {
namespace {

bool isLiteralTrue(const semantic::BoundExpression& expression) {
    if (expression.kind() != semantic::BoundNodeKind::LiteralExpression ||
        expression.type != semantic::PrimitiveType::Bool) return false;
    const auto& literal = static_cast<const semantic::BoundLiteralExpression&>(expression);
    return std::holds_alternative<bool>(literal.value) && std::get<bool>(literal.value);
}

} // namespace
''',
    '''namespace realscript::mir {
''')
patch(
    "src/mir/MirLowerer.cpp",
    '''        if (value.initializer) collectLocalTypes(*value.initializer); collectLocalTypes(*value.body); return;
''',
    '''        if (value.initializer) {
            collectLocalTypes(*value.initializer);
        }
        collectLocalTypes(*value.body);
        return;
''')
patch(
    "src/mir/MirLowerer.cpp",
    '''        if (value.initializer) emitStoreLocal(value.variable.index, lowerExpression(*value.initializer), statement.span); return;
''',
    '''        if (value.initializer) {
            emitStoreLocal(
                value.variable.index,
                lowerExpression(*value.initializer),
                statement.span);
        }
        return;
''')
patch(
    "tests/phase11_17_language_expansion_tests.cpp",
    '''    require(compilation.languageExpansions().size() == 1 &&
            compilation.languageExpansions().front().changed,
        "default language expansion did not run");
''',
    '''    require(compilation.languageExpansions().size() == 1 &&
            !compilation.languageExpansions().front().changed,
        "native control flow unexpectedly used source expansion");
''')

print("Phase 18A compile fixes applied")
