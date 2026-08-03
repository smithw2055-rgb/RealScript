#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "src/syntax/Parser.cpp"
text = path.read_text(encoding="utf-8")
old = '''            if (current().kind == SyntaxKind::OpenParenToken ||
                current().kind == SyntaxKind::SemicolonToken) {
'''
new = '''            const auto isAbstract = findModifier(
                memberModifiers, SyntaxKind::AbstractKeyword).has_value();
            if (current().kind == SyntaxKind::OpenParenToken ||
                (isAbstract && current().kind == SyntaxKind::SemicolonToken)) {
'''
count = text.count(old)
if count != 2:
    raise RuntimeError(f"expected two class/struct member anchors, found {count}")
path.write_text(text.replace(old, new), encoding="utf-8")
