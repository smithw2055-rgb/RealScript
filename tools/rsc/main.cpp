#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
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

std::string bytecodeArtifactName(std::string moduleName) {
    if (moduleName.empty()) moduleName = "module";
    for (auto& character : moduleName) {
        switch (character) {
        case '<': case '>': case ':': case '"':
        case '/': case '\\': case '|': case '?': case '*':
            character = '_';
            break;
        default:
            break;
        }
    }
    return moduleName + ".rsbc";
}

void printUsage() {
    std::cerr
        << "usage: rsc <file.rs>... "
           "[--check|--tokens|--mir|--symbols|--bytecode]\n"
        << "       rsc <file.rs>... --emit-bytecode <module.rsbc>\n"
        << "       rsc <file.rs>... --emit-bytecode-dir <directory>\n"
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
    realscript::optimization::Options optimizationOptions;
    optimizationOptions.level = realscript::optimization::Level::None;
    bool optimizationReport = false;
    bool deterministic = false;
    bool profile = false;
    bool printDigest = false;
    std::vector<std::string> paths;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--emit-bytecode" ||
            argument == "--emit-bytecode-dir" || argument == "--run") {
            mode = argument;
            if (index + 1 >= argc) {
                printUsage();
                return 2;
            }
            outputPath = argv[++index];
        } else if (argument == "--opt-level") {
            if (index + 1 >= argc) {
                printUsage();
                return 2;
            }
            const std::string level = argv[++index];
            if (level == "0") optimizationOptions.level = realscript::optimization::Level::None;
            else if (level == "1") optimizationOptions.level = realscript::optimization::Level::Basic;
            else if (level == "2") optimizationOptions.level = realscript::optimization::Level::Aggressive;
            else {
                std::cerr << "--opt-level must be 0, 1, or 2\n";
                return 2;
            }
        } else if (argument == "--opt-report") {
            optimizationReport = true;
        } else if (argument == "--deterministic") {
            deterministic = true;
        } else if (argument == "--profile") {
            profile = true;
        } else if (argument == "--digest") {
            printDigest = true;
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
        if (!result.diagnostics.hasErrors() &&
            optimizationOptions.level != realscript::optimization::Level::None) {
            realscript::optimization::Optimizer optimizer;
            auto optimized = optimizer.optimize(
                std::move(result.modules), result.diagnostics, optimizationOptions);
            result.modules = std::move(optimized.modules);
            if (optimizationReport) {
                std::cerr << realscript::optimization::levelName(
                    optimizationOptions.level) << ' '
                    << realscript::optimization::formatStatistics(
                        optimized.statistics) << '\n';
            }
        } else if (optimizationReport) {
            std::cerr << "O0 functions=0 iterations=0 constants-folded=0 "
                "local-loads-folded=0 branches-folded=0 "
                "instructions-removed=0 blocks-removed=0\n";
        }

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
                realscript::runtime::ExecutionOptions executionOptions;
                if (deterministic) {
                    executionOptions.determinism.mode =
                        realscript::runtime::DeterminismMode::Strict;
                }
                if (profile) {
                    executionOptions.profile =
                        std::make_shared<realscript::runtime::ProfileCollector>();
                }
                const auto execution = interpreter.invoke(
                    outputPath, {}, executionOptions);
                if (execution.succeeded) {
                    std::cout << realscript::runtime::valueToString(
                        execution.value,
                        interpreter.heap().get()) << '\n';
                    if (printDigest || deterministic) {
                        std::cerr << "determinism-digest="
                            << execution.determinismDigest << '\n';
                    }
                    if (executionOptions.profile) {
                        std::cerr << realscript::runtime::formatProfile(
                            executionOptions.profile->snapshot());
                    }
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
            (mode == "--bytecode" || mode == "--emit-bytecode" ||
             mode == "--emit-bytecode-dir")) {
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
                } else if (mode == "--emit-bytecode") {
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
                } else {
                    std::error_code directoryError;
                    std::filesystem::create_directories(
                        outputPath, directoryError);
                    if (directoryError) {
                        result.diagnostics.report(
                            "RS5201",
                            "failed to create bytecode output directory: " +
                                directoryError.message(),
                            {});
                    } else {
                        for (const auto& module : modules) {
                            const auto artifact =
                                std::filesystem::path(outputPath) /
                                bytecodeArtifactName(module.name);
                            writeBinaryFile(
                                artifact.string(),
                                realscript::bytecode::encodeModule(module));
                        }
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
#include <filesystem>
