#include "realscript/Version.h"
#include "realscript/game/GameScripting.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open input header: " + path.string());
    }
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot open output file: " + path.string());
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string escapeJson(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

void usage() {
    std::cerr
        << "usage: rsbind [options] <header.h>...\n"
        << "  --module <Module.Name>         Target RealScript module name (default: Engine.Native)\n"
        << "  --output-cpp <path.h>          Output C++ registration & ECS column binding header\n"
        << "  --output-drs <path.d.rs>       Output RealScript .d.rs declaration stub for LSP/Compiler\n"
        << "  --output-project <path.json>   Output realscript.project.json with embedded ambientStubs\n"
        << "  --project-name <name>          Project name for realscript.project.json (default: GameProject)\n"
        << "  --help\n"
        << "  --version\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    std::string moduleName = "Engine.Native";
    std::string projectName = "GameProject";
    std::filesystem::path outputCpp;
    std::filesystem::path outputDrs;
    std::filesystem::path outputProject;
    std::vector<std::filesystem::path> headers;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&](const char* opt) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string(opt) + " requires a value");
            }
            return argv[++i];
        };
        if (arg == "--help") {
            usage();
            return 0;
        } else if (arg == "--version") {
            std::cout << "rsbind " << realscript::kVersionString << "\n";
            return 0;
        } else if (arg == "--module") {
            moduleName = requireValue("--module");
        } else if (arg == "--project-name") {
            projectName = requireValue("--project-name");
        } else if (arg == "--output-cpp") {
            outputCpp = requireValue("--output-cpp");
        } else if (arg == "--output-drs") {
            outputDrs = requireValue("--output-drs");
        } else if (arg == "--output-project") {
            outputProject = requireValue("--output-project");
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "rsbind: unknown option: " << arg << "\n";
            return 2;
        } else {
            headers.emplace_back(arg);
        }
    }

    if (headers.empty()) {
        usage();
        return 2;
    }

    try {
        std::ostringstream combinedHeaders;
        for (const auto& hdr : headers) {
            combinedHeaders << readFile(hdr) << "\n";
        }

        const auto generated = realscript::game::GameApi::generateBindingsFromCppHeader(
            combinedHeaders.str(), moduleName);

        if (!outputCpp.empty()) {
            writeFile(outputCpp, generated.cppRegistrationSource);
        }
        if (!outputDrs.empty()) {
            writeFile(outputDrs, generated.scriptDeclarationStub);
        }
        if (!outputProject.empty()) {
            const std::string stubFilename = outputDrs.empty()
                ? (moduleName + ".d.rs")
                : outputDrs.filename().string();
            std::ostringstream json;
            json << "{\n"
                 << "  \"projectName\": \"" << escapeJson(projectName) << "\",\n"
                 << "  \"sdkVersion\": \"" << escapeJson(realscript::kVersionString) << "\",\n"
                 << "  \"nativeModules\": [\"" << escapeJson(moduleName) << "\"],\n"
                 << "  \"ambientStubs\": [\n"
                 << "    {\"path\": \"" << escapeJson(stubFilename)
                 << "\", \"content\": \"" << escapeJson(generated.scriptDeclarationStub) << "\"}\n"
                 << "  ]\n"
                 << "}\n";
            writeFile(outputProject, json.str());
        }

        std::cout << "rsbind: module '" << moduleName << "' -> exported "
                  << generated.classesExported << " classes, "
                  << generated.structsExported << " ECS structs, "
                  << generated.methodsExported << " methods, "
                  << generated.propertiesExported << " properties, "
                  << generated.functionsExported << " functions, "
                  << generated.enumsExported << " enums.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "rsbind: " << ex.what() << "\n";
        return 1;
    }
}
