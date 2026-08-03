#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "tests/phase18_native_control_flow_tests.cpp"
text = path.read_text(encoding="utf-8")
old = '''    require(!diagnostics.hasErrors(),
        "native event syntax failed to parse:
" +
            diagnosticsText(diagnostics));
'''
new = '''    require(!diagnostics.hasErrors(),
        "native event syntax failed to parse:\\n" +
            diagnosticsText(diagnostics));
'''
if new not in text:
    if old not in text:
        raise RuntimeError("event syntax diagnostic string anchor not found")
    text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
