#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
path = root / "src/syntax/SyntaxNodes.cpp"
text = path.read_text(encoding="utf-8")
old_local = '''text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {
    return combine(declarationStart(attributes, type.span()), semicolonToken.span);
}
'''
new_local = '''text::TextSpan VariableDeclarationStatementSyntax::span() const noexcept {
    return combine(type.span(), semicolonToken.span);
}
'''
old_field = '''text::TextSpan FieldDeclarationSyntax::span() const noexcept {
    return combine(type.span(), semicolonToken.span);
}
'''
new_field = '''text::TextSpan FieldDeclarationSyntax::span() const noexcept {
    return combine(
        declarationStart(attributes, type.span()),
        semicolonToken.span);
}
'''
if old_local not in text or old_field not in text:
    raise RuntimeError("attribute span repair anchors missing")
text = text.replace(old_local, new_local, 1)
text = text.replace(old_field, new_field, 1)
path.write_text(text, encoding="utf-8", newline="\n")
print("Native attribute declaration spans repaired")
