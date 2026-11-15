#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/compiler/ProjectConfig.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/Version.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
            << ": "
            << (diagnostic.severity == realscript::diagnostics::DiagnosticSeverity::Error
                ? "error"
                : "warning")
            << ' ' << diagnostic.code << ": "
            << diagnostic.message << '\n';
    }
}

bool endsWithIgnoreCase(std::string_view text, std::string_view lowerSuffix) noexcept {
    if (text.size() < lowerSuffix.size()) return false;
    const std::size_t offset = text.size() - lowerSuffix.size();
    for (std::size_t i = 0; i < lowerSuffix.size(); ++i) {
        char ch = text[offset + i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (ch != lowerSuffix[i]) return false;
    }
    return true;
}

bool hasValidScriptExtension(const std::string& path) noexcept {
    return endsWithIgnoreCase(path, ".d.rscript") ||
        endsWithIgnoreCase(path, ".rscript");
}

void requireScriptExtension(const std::string& path) {
    if (!hasValidScriptExtension(path)) {
        throw std::runtime_error(
            "RealScript source files must use the .rscript or .d.rscript extension: " + path);
    }
}

void usage() {
    std::cerr
        << "usage: rsaot [options] <file.rscript>...\n"
        << "  --output-dir <directory>\n"
        << "  --project <file>\n"
        << "  --no-project-config\n"
        << "  --pack <file-or-dir>\n"
        << "  --plugin <shared-library>\n"
        << "  --cache-dir <directory>\n"
        << "  --cache-stats\n"
        << "  --program-name <identifier>\n"
        << "  --namespace <identifier>\n"
        << "  --query-symbol <identifier>\n"
        << "  --opt-level <0|1|2>\n"
        << "  --opt-report\n"
        << "  --no-line-directives\n"
        << "  --no-shared-generics\n"
        << "  --trusted-release\n"
        << "  --help\n"
        << "  --version\n";
}

void collectPackInputs(
    const std::string& rawPath,
    std::vector<std::filesystem::path>& outPacks) {
    const std::filesystem::path p(rawPath);
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        std::vector<std::filesystem::path> entries;
        for (const auto& entry : std::filesystem::directory_iterator(p, ec)) {
            if (ec || !entry.is_regular_file(ec)) continue;
            const auto utf8 = entry.path().string();
            if (endsWithIgnoreCase(utf8, ".d.rscript") ||
                endsWithIgnoreCase(utf8, ".rscript") ||
                endsWithIgnoreCase(utf8, ".json")) {
                entries.push_back(entry.path());
            }
        }
        std::sort(entries.begin(), entries.end());
        outPacks.insert(outPacks.end(), entries.begin(), entries.end());
        return;
    }
    outPacks.push_back(p);
}

[[nodiscard]] std::uint64_t hashPathList(
    const std::vector<std::filesystem::path>& files,
    const std::vector<std::filesystem::path>& plugins) {
    std::uint64_t h = 14695981039346656037ULL;
    const auto mixStr = [&](std::string_view s) {
        for (const char ch : s) {
            h ^= static_cast<unsigned char>(ch);
            h *= 1099511628211ULL;
        }
    };
    for (const auto& f : files) {
        mixStr("pack:");
        mixStr(f.string());
        std::ifstream in(f, std::ios::binary);
        if (in) {
            std::ostringstream oss;
            oss << in.rdbuf();
            mixStr(oss.str());
        }
    }
    for (const auto& p : plugins) {
        mixStr("plugin:");
        mixStr(p.string());
        std::ifstream in(p, std::ios::binary);
        if (in) {
            std::ostringstream oss;
            oss << in.rdbuf();
            mixStr(oss.str());
        }
    }
    return h == 0 ? 1ULL : h;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    try {
        std::filesystem::path outputDirectory = ".";
        std::filesystem::path cacheDirectory;
        std::string projectConfigPath;
        bool noProjectConfig = false;
        bool printCacheStats = false;
        realscript::aot::GenerationOptions options;
        realscript::optimization::Options optimizationOptions;
        optimizationOptions.level = realscript::optimization::Level::Aggressive;
        bool optimizationReport = false;
        std::vector<std::string> sources;
        std::vector<std::string> cliPacks;
        std::vector<std::string> cliPlugins;
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
                std::cout << "rsaot " << realscript::kVersionString
                    << " (Runtime ABI "
                    << realscript::aot::RuntimeAbiMajor << '.'
                    << realscript::aot::RuntimeAbiMinor << ")\n";
                return 0;
            } else if (argument == "--output-dir") {
                outputDirectory = requireValue("--output-dir");
            } else if (argument == "--project") {
                projectConfigPath = requireValue("--project");
                if (projectConfigPath.empty()) {
                    throw std::runtime_error("--project requires a value");
                }
            } else if (argument.rfind("--project=", 0) == 0) {
                projectConfigPath = argument.substr(10);
                if (projectConfigPath.empty()) {
                    throw std::runtime_error("--project requires a value");
                }
            } else if (argument == "--no-project-config") {
                noProjectConfig = true;
            } else if (argument == "--pack") {
                const auto val = requireValue("--pack");
                if (val.empty()) throw std::runtime_error("--pack requires a value");
                cliPacks.push_back(val);
            } else if (argument.rfind("--pack=", 0) == 0) {
                const auto val = argument.substr(7);
                if (val.empty()) throw std::runtime_error("--pack requires a value");
                cliPacks.push_back(val);
            } else if (argument == "--plugin") {
                const auto val = requireValue("--plugin");
                if (val.empty()) throw std::runtime_error("--plugin requires a value");
                cliPlugins.push_back(val);
            } else if (argument.rfind("--plugin=", 0) == 0) {
                const auto val = argument.substr(9);
                if (val.empty()) throw std::runtime_error("--plugin requires a value");
                cliPlugins.push_back(val);
            } else if (argument == "--cache-dir") {
                cacheDirectory = requireValue("--cache-dir");
            } else if (argument.rfind("--cache-dir=", 0) == 0) {
                cacheDirectory = argument.substr(12);
                if (cacheDirectory.empty()) {
                    throw std::runtime_error("--cache-dir requires a value");
                }
            } else if (argument == "--cache-stats") {
                printCacheStats = true;
            } else if (argument == "--program-name") {
                options.programName = requireValue("--program-name");
            } else if (argument == "--namespace") {
                options.cppNamespace = requireValue("--namespace");
            } else if (argument == "--query-symbol") {
                options.querySymbol = requireValue("--query-symbol");
            } else if (argument == "--opt-level") {
                const auto level = requireValue("--opt-level");
                if (level == "0") optimizationOptions.level = realscript::optimization::Level::None;
                else if (level == "1") optimizationOptions.level = realscript::optimization::Level::Basic;
                else if (level == "2") optimizationOptions.level = realscript::optimization::Level::Aggressive;
                else throw std::runtime_error("--opt-level must be 0, 1, or 2");
            } else if (argument == "--opt-report") {
                optimizationReport = true;
            } else if (argument == "--no-line-directives") {
                options.emitLineDirectives = false;
            } else if (argument == "--no-shared-generics") {
                options.enableSharedReferenceGenerics = false;
            } else if (argument == "--trusted-release") {
                options.trustedRelease = true;
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

        std::vector<realscript::compiler::SourceFile> sourceFiles;
        sourceFiles.reserve(sources.size());
        for (const auto& path : sources) {
            requireScriptExtension(path);
            sourceFiles.push_back({path, readFile(path)});
        }

        realscript::compiler::CompilationOptions projectOptions;
        realscript::diagnostics::DiagnosticBag projectDiagnostics;
        std::optional<std::filesystem::path> explicitProjectPath;
        if (!projectConfigPath.empty()) {
            explicitProjectPath = std::filesystem::path(projectConfigPath);
        }
        if (!realscript::compiler::resolveAndApplyProjectConfigForInputs(
                sourceFiles,
                explicitProjectPath,
                noProjectConfig,
                projectOptions,
                &projectDiagnostics) ||
            projectDiagnostics.hasErrors()) {
            printDiagnostics(projectDiagnostics);
            return 1;
        }
        if (!cliPacks.empty() || !cliPlugins.empty()) {
            realscript::compiler::ProjectConfig cliConfig;
            for (const auto& p : cliPacks) {
                collectPackInputs(p, cliConfig.extraPackFiles);
            }
            for (const auto& p : cliPlugins) {
                cliConfig.nativeMacroPluginPaths.emplace_back(p);
            }
            cliConfig.configFingerprint =
                hashPathList(cliConfig.extraPackFiles, cliConfig.nativeMacroPluginPaths);
            const std::string savedProjectPath = projectOptions.projectConfigPath;
            if (!realscript::compiler::applyProjectConfigToOptions(
                    cliConfig, projectOptions, &projectDiagnostics) ||
                projectDiagnostics.hasErrors()) {
                printDiagnostics(projectDiagnostics);
                return 1;
            }
            if (!savedProjectPath.empty()) {
                projectOptions.projectConfigPath = savedProjectPath;
            }
        }
        printDiagnostics(projectDiagnostics);

        if (!cacheDirectory.empty() || printCacheStats) {
            realscript::compiler::ModuleBuildCacheOptions cacheOptions;
            cacheOptions.cacheDirectory = cacheDirectory;
            cacheOptions.optimizationOptions = optimizationOptions;
            if (noProjectConfig) {
                cacheOptions.flags |= realscript::compiler::kBuildCacheFlagNoProjectConfig;
            }

            realscript::compiler::ScopedProjectCompilationOptions scopedProject(projectOptions);
            realscript::compiler::ModuleBuildCache buildCache(cacheDirectory);
            auto cached = buildCache.buildAot(sourceFiles, cacheOptions, options);
            if (printCacheStats) {
                std::cerr << realscript::compiler::ModuleBuildCache::formatStatistics(
                    cached.statistics) << '\n';
            }
            if (!cached.succeeded()) {
                printDiagnostics(cached.diagnostics);
                return 1;
            }
            if (optimizationReport) {
                std::cerr << realscript::optimization::levelName(optimizationOptions.level)
                    << ' ' << realscript::optimization::formatStatistics(
                        cached.optimizationStatistics) << '\n';
            }

            std::filesystem::create_directories(outputDirectory);
            const bool headerChanged = writeFileIfChanged(
                outputDirectory / "realscript_aot_generated.h", cached.program.header);
            const bool sourceChanged = writeFileIfChanged(
                outputDirectory / "realscript_aot_generated.cpp", cached.program.source);
            const bool manifestChanged = writeFileIfChanged(
                outputDirectory / "realscript_aot_manifest.json", cached.program.manifest);
            std::cout << "generated RealScript C++17 AOT program '"
                << realscript::aot::sanitizeCppIdentifier(options.programName)
                << "' with content hash 0x"
                << std::hex << cached.program.contentHash << std::dec
                << (headerChanged || sourceChanged || manifestChanged
                    ? " (updated)\n"
                    : " (unchanged)\n");
            return 0;
        }

        realscript::compiler::Compilation compilation(sourceFiles, projectOptions);
        auto build = compilation.build();
        if (build.diagnostics.hasErrors()) {
            printDiagnostics(build.diagnostics);
            return 1;
        }

        realscript::optimization::Optimizer optimizer;
        auto optimized = optimizer.optimize(
            std::move(build.modules), build.diagnostics, optimizationOptions);
        if (build.diagnostics.hasErrors()) {
            printDiagnostics(build.diagnostics);
            return 1;
        }
        if (optimizationReport) {
            std::cerr << realscript::optimization::levelName(optimizationOptions.level)
                << ' ' << realscript::optimization::formatStatistics(
                    optimized.statistics) << '\n';
        }

        realscript::aot::CppGenerator generator;
        auto generated = generator.generate(optimized.modules, build.diagnostics, options);
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
