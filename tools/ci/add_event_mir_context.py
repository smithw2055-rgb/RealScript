#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "src/mir/MirExpressions.cpp"
text = path.read_text(encoding="utf-8")
old = '''    if (!hasCurrentBlock() || currentBlockTerminated()) {
        throw std::logic_error(
            "cannot emit a value without an open MIR block");
    }
'''
new = '''    if (!hasCurrentBlock() || currentBlockTerminated()) {
        const auto functionName = currentFunction_
            ? currentFunction_->name
            : std::string{"<none>"};
        throw std::logic_error(
            "cannot emit MIR value '" +
            std::string{opcodeName(opcode)} +
            "' without an open block in function '" +
            functionName + "'");
    }
'''
if new not in text:
    if old not in text:
        raise RuntimeError("MIR emitValue diagnostic anchor not found")
    text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
