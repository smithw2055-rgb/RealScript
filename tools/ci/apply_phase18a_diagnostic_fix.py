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
    "tests/phase11_17_language_expansion_tests.cpp",
    '''        realscript::diagnostics::DiagnosticBag diagnostics;
        require(realscript::bytecode::verifyModule(module, diagnostics),
            "extended bytecode verification failed:\\n" + diagnosticsText(diagnostics));
        modules.push_back(std::move(module));
''',
    '''        realscript::diagnostics::DiagnosticBag diagnostics;
        const auto verified =
            realscript::bytecode::verifyModule(module, diagnostics);
        require(verified,
            "extended bytecode verification failed:\\n" +
                diagnosticsText(diagnostics));
        modules.push_back(std::move(module));
''')

patch(
    "tests/phase18_native_control_flow_tests.cpp",
    '''    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) modules.push_back(lowerer.lower(module));
    realscript::runtime::Interpreter interpreter(std::move(modules));
''',
    '''    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        auto module = lowerer.lower(sourceModule);
        realscript::diagnostics::DiagnosticBag diagnostics;
        const auto verified =
            realscript::bytecode::verifyModule(module, diagnostics);
        require(verified,
            "native bytecode verification failed:\\n" +
                diagnosticsText(diagnostics));
        modules.push_back(std::move(module));
    }
    realscript::runtime::Interpreter interpreter(std::move(modules));
''')

print("Phase 18A verifier diagnostics fixed")
