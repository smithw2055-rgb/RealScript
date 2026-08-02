#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "docs/roadmap/PHASE_18_24_NATIVE_LANGUAGE_AND_RUNTIME.md"
text = path.read_text(encoding="utf-8")
text = text.replace(
    "### 18F — reference modifiers and exact aliases — pending",
    "### 18F — reference modifiers and exact aliases")
path.write_text(text, encoding="utf-8")
