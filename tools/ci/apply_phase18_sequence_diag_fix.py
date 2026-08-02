#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
path = root / "src/semantic/SemanticBinding.cpp"
text = path.read_text(encoding="utf-8")
old = '''    case syntax::SyntaxKind::YieldWaitStatement:
        diagnostics_.report(
            "RS2494",
            "yield wait_ticks is valid only at sequence top level",
            syntaxTree.span());
        return std::make_unique<BoundExpressionStatement>();
'''
new = '''    case syntax::SyntaxKind::YieldWaitStatement: {
        diagnostics_.report(
            "RS2494",
            "yield wait_ticks is valid only at sequence top level",
            syntaxTree.span());
        auto result = std::make_unique<BoundExpressionStatement>();
        result->span = syntaxTree.span();
        result->expression = makeError(syntaxTree.span());
        return result;
    }
'''
if old not in text:
    raise RuntimeError("yield diagnostic anchor missing")
path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
print("Native sequence diagnostic placeholder fixed")
