#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

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
    std::cerr << "usage: rsc <file.rs> [--tokens|--mir]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        printUsage();
        return 2;
    }

    const std::string path = argv[1];
    const std::string mode = argc == 3 ? argv[2] : "--check";

    try {
        realscript::text::SourceText source(readFile(path), path);
        realscript::diagnostics::DiagnosticBag diagnostics;

        if (mode == "--tokens") {
            realscript::syntax::Lexer lexer(source, diagnostics);
            for (const auto& token : lexer.lexAll()) {
                std::cout << token.span.start << ':' << token.span.length << ' '
                          << realscript::syntax::syntaxKindName(token.kind);
                if (!token.text.empty()) {
                    std::cout << " `" << token.text << '`';
                }
                std::cout << '\n';
            }
        } else {
            realscript::syntax::Parser parser(source, diagnostics);
            auto syntaxTree = parser.parseCompilationUnit();
            realscript::semantic::Binder binder(diagnostics);
            auto semanticModel = binder.bind(syntaxTree);

            if (mode == "--mir" && !diagnostics.hasErrors()) {
                realscript::mir::Lowerer lowerer;
                const auto module = lowerer.lower(semanticModel);
                (void)realscript::mir::verifyModule(module, diagnostics);
                if (!diagnostics.hasErrors()) {
                    std::cout << realscript::mir::printModule(module);
                }
            } else if (mode != "--check") {
                printUsage();
                return 2;
            }
        }

        for (const auto& diagnostic : diagnostics.items()) {
            std::cerr << realscript::diagnostics::formatDiagnostic(diagnostic, source) << '\n';
        }
        return diagnostics.hasErrors() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "rsc: " << error.what() << '\n';
        return 2;
    }
}
