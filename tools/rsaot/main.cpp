#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open input file: " + path);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

bool writeFileIfChanged(
    const std::filesystem::path& path,
    const std::string& content) {
    {
        std::ifstream current(path, std::ios::binary);
        if (current) {
            std::ostringstream existing;
            existing << current.rdbuf();
            if (existing.str() == content) return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot open output file: " + path.string());
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!stream) {
        throw std::runtime_error("cannot write output file: " + path.string());
    }
    return true;
}

void printDiagnostics(const realscript::diagnostics::DiagnosticBag& diagnostics) {
    for (const auto& diagnostic : diagnostics.items()) {
        std::cerr << (diagnostic.sourceName.empty() ? "<aot>" : diagnostic.sourceName)
            << ": error " << diagnostic.code << ": "
            << diagnostic.message << '\n';
    }
}

void usage() {
    std::cerr
        << "usage: rsaot [options] <file.rs>...\n"
        << "  --output-dir <directory>\n"
        << "  --program-name <identifier>\n"
        << "  --namespace <identifier>\n"
        << "  --query-symbol <identifier>\n"
        << "  --no-line-directives\n"
        << "  --help\n"
        << "  --version\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    std::filesystem::path outputDirectory = ".";
    realscript::aot::GenerationOptions options;
    std::vector<std::string> sources;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto requireValue = [&](const char* option) -> std::string {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string(option) + " requires a value");
            }
            return argv[++index];
        };
        if (argument == "--help") {
            usage();
            return 0;
        } else if (argument == "--version") {
            std::cout << "rsaot 0.1 (Runtime ABI "
                << realscript::aot::RuntimeAbiMajor << '.'
                << realscript::aot::RuntimeAbiMinor << ")\n";
            return 0;
        } else if (argument == "--output-dir") {
            outputDirectory = requireValue("--output-dir");
        } else if (argument == "--program-name") {
            options.programName = requireValue("--program-name");
        } else if (argument == "--namespace") {
            options.cppNamespace = requireValue("--namespace");
        } else if (argument == "--query-symbol") {
            options.querySymbol = requireValue("--query-symbol");
        } else if (argument == "--no-line-directives") {
            options.emitLineDirectives = false;
        } else if (argument.rfind("--", 0) == 0) {
            throw std::runtime_error("unknown option: " + argument);
        } else {
            sources.push_back(argument);
        }
    }
    if (sources.empty()) {
        usage();
        return 2;
    }

    try {
        realscript::compiler::Compilation compilation;
        for (const auto& path : sources) {
            compilation.addSource({path, readFile(path)});
        }
        auto build = compilation.build();
        if (build.diagnostics.hasErrors()) {
            printDiagnostics(build.diagnostics);
            return 1;
        }

        realscript::aot::CppGenerator generator;
        auto generated = generator.generate(build.modules, build.diagnostics, options);
        if (build.diagnostics.hasErrors()) {
            printDiagnostics(build.diagnostics);
            return 1;
        }

        std::filesystem::create_directories(outputDirectory);
        const bool headerChanged = writeFileIfChanged(
            outputDirectory / "realscript_aot_generated.h", generated.header);
        const bool sourceChanged = writeFileIfChanged(
            outputDirectory / "realscript_aot_generated.cpp", generated.source);
        const bool manifestChanged = writeFileIfChanged(
            outputDirectory / "realscript_aot_manifest.json", generated.manifest);
        std::cout << "generated RealScript C++17 AOT program '"
            << realscript::aot::sanitizeCppIdentifier(options.programName)
            << "' with content hash 0x"
            << std::hex << generated.contentHash << std::dec
            << (headerChanged || sourceChanged || manifestChanged
                ? " (updated)\n"
                : " (unchanged)\n");
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "rsaot: " << exception.what() << '\n';
        return 2;
    }
}
