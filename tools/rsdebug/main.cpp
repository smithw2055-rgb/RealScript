#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DapServer.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <cstdlib>
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
    std::uint16_t tcpPort = 0;
    std::string bindHost = "127.0.0.1";
    std::vector<std::string> sourceFiles;

    const auto parsePortArg = [](const char* raw, std::uint16_t& outPort) -> bool {
        if (!raw || *raw == '\0') return false;
        char* endPtr = nullptr;
        const unsigned long val = std::strtoul(raw, &endPtr, 10);
        if (endPtr == raw || *endPtr != '\0' || val == 0 || val > 65535UL) {
            return false;
        }
        outPort = static_cast<std::uint16_t>(val);
        return true;
    };

    for (int index = 1; index < argc; ++index) {
        std::string arg = argv[index];
        if ((arg == "--port" || arg == "--dap-port") && index + 1 < argc) {
            if (!parsePortArg(argv[++index], tcpPort)) {
                std::cerr << "rsdebug: invalid TCP port '" << argv[index] << "'\n";
                return 2;
            }
        } else if (arg.rfind("--port=", 0) == 0) {
            if (!parsePortArg(arg.c_str() + 7, tcpPort)) {
                std::cerr << "rsdebug: invalid TCP port '" << (arg.c_str() + 7) << "'\n";
                return 2;
            }
        } else if (arg.rfind("--dap-port=", 0) == 0) {
            if (!parsePortArg(arg.c_str() + 11, tcpPort)) {
                std::cerr << "rsdebug: invalid TCP port '" << (arg.c_str() + 11) << "'\n";
                return 2;
            }
        } else if (arg == "--host" && index + 1 < argc) {
            bindHost = argv[++index];
        } else if (arg.rfind("--host=", 0) == 0) {
            bindHost = arg.substr(7);
        } else {
            sourceFiles.push_back(std::move(arg));
        }
    }

    if (sourceFiles.empty()) {
        std::cerr << "usage: rsdebug [--port <port>] [--host <ip>] <file.rs>...\n";
        return 2;
    }
    try {
        realscript::compiler::Compilation compilation;
        for (const auto& file : sourceFiles) {
            compilation.addSource({file, readFile(file)});
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
        if (tcpPort != 0) {
            return server.serveTcp(tcpPort, bindHost);
        }
        return server.run(std::cin, std::cout);
    } catch (const std::exception& error) {
        std::cerr << "rsdebug: " << error.what() << '\n';
        return 2;
    }
}
