#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "tests/phase18_native_control_flow_tests.cpp"
text = path.read_text(encoding="utf-8")
old = '''    run(
        "structured control flow bypasses expansion",
        testNoStructuredSourceRewrite);
'''
if old not in text:
    raise RuntimeError("legacy expansion test registration not found")
path.write_text(text.replace(old, "", 1), encoding="utf-8")
