#!/usr/bin/env python3
from pathlib import Path

path = Path(__file__).resolve().parents[2] / "src/mir/MirExpressions.cpp"
text = path.read_text(encoding="utf-8")
old = '''        throw std::logic_error(
            "cannot emit MIR value '" +
            std::string{opcodeName(opcode)} +
            "' without an open block in function '" +
            functionName + "'");
'''
new = '''        const auto blockState = !hasCurrentBlock()
            ? std::string{"none"}
            : std::string{"bb"} + std::to_string(*currentBlockId_) +
                ":" + terminatorName(
                    block(*currentBlockId_).terminator.kind);
        throw std::logic_error(
            "cannot emit MIR value '" +
            std::string{opcodeName(opcode)} +
            "' at span " + std::to_string(sourceSpan.start) +
            ":" + std::to_string(sourceSpan.length) +
            " with block " + blockState +
            " in function '" + functionName + "'");
'''
if new not in text:
    if old not in text:
        raise RuntimeError("detailed MIR diagnostic anchor not found")
    text = text.replace(old, new, 1)
path.write_text(text, encoding="utf-8")
