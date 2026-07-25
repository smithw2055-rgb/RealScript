#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open input file: " + path);
    }
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

std::vector<std::uint8_t> readBinaryFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("cannot open input file: " + path);
    }
    const auto end = stream.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine input size: " + path);
    }
    const auto size = static_cast<std::size_t>(end);
    std::vector<std::uint8_t> bytes(size);
    stream.seekg(0, std::ios::beg);
    if (size != 0) {
        stream.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(size));
    }
    if (!stream) {
        throw std::runtime_error("cannot read input file: " + path);
    }
    return bytes;
}

void writeBinaryFile(
    const std::string& path,
    const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot open output file: " + path);
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw std::runtime_error("cannot write output file: " + path);
    }
}

void printUsage() {
    std::cerr
        << "usage: rsc <file.rs>... "
           "[--check|--tokens|--mir|--symbols|--bytecode]\n"
        << "       rsc <file.rs>... --emit-bytecode <module.rsbc>\n"
        << "       rsc <module.rsbc> --disassemble\n"
        << "       rsc <file.rs>... --run <module::function>\n";
}

void printDiagnostics(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    for (const auto& diagnostic : diagnostics.items()) {
        std::cerr
            << (diagnostic.sourceName.empty()
                ? "<compilation>"
                : diagnostic.sourceName)
            << ": "
            << (diagnostic.severity ==
                    realscript::diagnostics::DiagnosticSeverity::Error
                ? "error"
                : "warning")
            << ' ' << diagnostic.code << ": "
            << diagnostic.message << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }

    std::string mode = "--check";
    std::string outputPath;
    std::vector<std::string> paths;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--emit-bytecode" || argument == "--run") {
            mode = argument;
            if (index + 1 >= argc) {
                printUsage();
                return 2;
            }
            outputPath = argv[++index];
        } else if (argument.rfind("--", 0) == 0) {
            mode = argument;
        } else {
            paths.push_back(argument);
        }
    }

    if (paths.empty()) {
        printUsage();
        return 2;
    }

    try {
        if (mode == "--tokens") {
            if (paths.size() != 1) {
                std::cerr << "--tokens accepts exactly one source file\n";
                return 2;
            }

            realscript::text::SourceText source(
                readFile(paths.front()),
                paths.front());
            realscript::diagnostics::DiagnosticBag diagnostics;
            realscript::syntax::Lexer lexer(source, diagnostics);
            for (const auto& token : lexer.lexAll()) {
                std::cout << token.span.start << ':' << token.span.length << ' '
                    << realscript::syntax::syntaxKindName(token.kind);
                if (!token.text.empty()) {
                    std::cout << " `" << token.text << '`';
                }
                std::cout << '\n';
            }
            for (const auto& diagnostic : diagnostics.items()) {
                std::cerr << realscript::diagnostics::formatDiagnostic(
                    diagnostic,
                    source) << '\n';
            }
            return diagnostics.hasErrors() ? 1 : 0;
        }

        if (mode == "--disassemble") {
            if (paths.size() != 1) {
                std::cerr << "--disassemble accepts exactly one .rsbc file\n";
                return 2;
            }
            realscript::bytecode::Module module;
            realscript::diagnostics::DiagnosticBag diagnostics;
            if (realscript::bytecode::decodeModule(
                    readBinaryFile(paths.front()),
                    module,
                    diagnostics)) {
                (void)realscript::bytecode::verifyModule(module, diagnostics);
            }
            if (!diagnostics.hasErrors()) {
                std::cout << realscript::bytecode::disassembleModule(module);
            }
            printDiagnostics(diagnostics);
            return diagnostics.hasErrors() ? 1 : 0;
        }

        realscript::compiler::Compilation compilation;
        for (const auto& path : paths) {
            compilation.addSource({path, readFile(path)});
        }
        auto result = compilation.build();

        if (!result.diagnostics.hasErrors() && mode == "--run") {
            std::vector<realscript::bytecode::Module> modules;
            realscript::bytecode::Lowerer lowerer;
            for (const auto& mirModule : result.modules) {
                auto module = lowerer.lower(mirModule);
                (void)realscript::bytecode::verifyModule(module, result.diagnostics);
                modules.push_back(std::move(module));
            }
            if (!result.diagnostics.hasErrors()) {
                realscript::runtime::Interpreter interpreter(std::move(modules));
                const auto execution = interpreter.invoke(outputPath);
                if (execution.succeeded) {
                    std::cout << realscript::runtime::valueToString(execution.value) << '\n';
                } else {
                    std::cerr << "runtime error "
                        << realscript::runtime::errorCodeName(execution.error.code)
                        << ": " << execution.error.message << '\n';
                    for (const auto& frame : execution.error.stackTrace) {
                        std::cerr << "  at " << frame << '\n';
                    }
                    return 1;
                }
            }
        } else if (!result.diagnostics.hasErrors() &&
            (mode == "--bytecode" || mode == "--emit-bytecode")) {
            std::vector<realscript::bytecode::Module> modules;
            realscript::bytecode::Lowerer lowerer;
            for (const auto& mirModule : result.modules) {
                auto module = lowerer.lower(mirModule);
                (void)realscript::bytecode::verifyModule(
                    module,
                    result.diagnostics);
                modules.push_back(std::move(module));
            }

            if (!result.diagnostics.hasErrors()) {
                if (mode == "--bytecode") {
                    for (const auto& module : modules) {
                        std::cout << realscript::bytecode::disassembleModule(module)
                            << '\n';
                    }
                } else {
                    if (modules.size() != 1) {
                        result.diagnostics.report(
                            "RS5200",
                            "--emit-bytecode requires exactly one compiled module",
                            {});
                    } else {
                        writeBinaryFile(
                            outputPath,
                            realscript::bytecode::encodeModule(modules.front()));
                    }
                }
            }
        } else if (mode == "--mir") {
            for (const auto& module : result.modules) {
                std::cout << realscript::mir::printModule(module);
            }
        } else if (mode == "--symbols") {
            for (const auto& module : result.modules) {
                for (const auto& function : module.functions) {
                    std::cout << module.name << "::" << function.name
                        << " 0x" << std::hex << function.symbolId
                        << std::dec << '\n';
                }
            }
        } else if (mode != "--check") {
            printUsage();
            return 2;
        }

        printDiagnostics(result.diagnostics);
        return result.diagnostics.hasErrors() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "rsc: " << error.what() << '\n';
        return 2;
    }
}
