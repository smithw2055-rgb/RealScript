#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DapServer.h"
#include "realscript/runtime/Runtime.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open source file: " + path);
    std::ostringstream output;
    output << stream.rdbuf();
    return output.str();
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: rsdebug <file.rs>...\n";
        return 2;
    }
    try {
        realscript::compiler::Compilation compilation;
        for (int index = 1; index < argc; ++index) {
            compilation.addSource({argv[index], readFile(argv[index])});
        }
        auto result = compilation.build();
        if (result.diagnostics.hasErrors()) {
            for (const auto& diagnostic : result.diagnostics.items()) {
                std::cerr << diagnostic.sourceName << ": " << diagnostic.code
                    << ": " << diagnostic.message << '\n';
            }
            return 1;
        }
        realscript::bytecode::Lowerer lowerer;
        std::vector<realscript::bytecode::Module> modules;
        for (const auto& mir : result.modules) modules.push_back(lowerer.lower(mir));
        realscript::runtime::RuntimeError error;
        auto program = realscript::runtime::ProgramImage::link(std::move(modules), error);
        if (!program) {
            std::cerr << "rsdebug: " << error.message << '\n';
            return 1;
        }
        auto image = std::make_shared<const realscript::runtime::ProgramImage>(std::move(*program));
        realscript::debug::DapServer server(std::move(image));
        return server.run(std::cin, std::cout);
    } catch (const std::exception& error) {
        std::cerr << "rsdebug: " << error.what() << '\n';
        return 2;
    }
}
