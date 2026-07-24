#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
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

void printUsage() {
    std::cerr
        << "usage: rsc <file.rs>... [--check|--tokens|--mir|--symbols]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }

    std::string mode = "--check";
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.rfind("--", 0) == 0) {
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

        realscript::compiler::Compilation compilation;
        for (const auto& path : paths) {
            compilation.addSource({path, readFile(path)});
        }
        const auto result = compilation.build();

        if (mode == "--mir") {
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

        for (const auto& diagnostic : result.diagnostics.items()) {
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
        return result.diagnostics.hasErrors() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "rsc: " << error.what() << '\n';
        return 2;
    }
}
