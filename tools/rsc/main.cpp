#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"
#include "realscript/tooling/Formatter.h"
#include "realscript/tooling/GovernanceLinter.h"
#include "realscript/tooling/Json.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

void writeTextFile(const std::string& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot open output file: " + path);
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        throw std::runtime_error("cannot write output file: " + path);
    }
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

void printUsage() {
    std::cerr
        << "usage: rsc <file.rscript>... "
           "[--check|--tokens|--mir|--symbols|--bytecode] "
           "[--cache-dir <directory>] [--cache-stats]\n"
        << "       rsc <file.rscript>... --format [--write|-i|--stdout|-o <output.rscript>]\n"
        << "       rsc <file.rscript>... --check-format\n"
        << "       rsc <file.rscript>... --lint [--lint-deny-warnings] [--project <realscript.project.json>]\n"
        << "       rsc <file.rscript>... --emit-bytecode <module.rsbc> "
           "[--cache-dir <directory>] [--cache-stats]\n"
        << "       rsc <file.rscript>... --emit-bytecode-dir <directory> "
           "[--cache-dir <directory>] [--cache-stats]\n"
        << "       rsc <module.rsbc> --disassemble\n"
        << "       rsc <file.rscript>... --run <module::function> "
           "[--cache-dir <directory>] [--cache-stats]\n";
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

void printLintDiagnostics(
    const std::vector<realscript::tooling::LintDiagnostic>& diagnostics) {
    for (const auto& diag : diagnostics) {
        std::cerr
            << (diag.sourcePath.empty() ? "<lint>" : diag.sourcePath)
            << ':' << (diag.range.start.line + 1)
            << ':' << (diag.range.start.column + 1)
            << ": " << realscript::tooling::lintSeverityName(diag.severity)
            << ' ' << diag.code
            << " [" << diag.ruleName << "]: "
            << diag.message << '\n';
    }
}

realscript::tooling::LintConfig discoverLintConfig(
    const std::string& explicitConfigPath,
    const std::vector<std::string>& sourcePaths) {
    realscript::tooling::LintConfig cfg;
    std::filesystem::path foundConfig;
    std::error_code ec;

    if (!explicitConfigPath.empty()) {
        foundConfig = std::filesystem::path(explicitConfigPath);
        if (!std::filesystem::is_regular_file(foundConfig, ec) || ec) {
            throw std::runtime_error("cannot open project config file: " + explicitConfigPath);
        }
    } else if (!sourcePaths.empty()) {
        std::filesystem::path cur = std::filesystem::absolute(std::filesystem::path(sourcePaths.front()), ec);
        if (!ec) {
            cur = cur.parent_path();
            for (int depth = 0; depth < 16 && !cur.empty(); ++depth) {
                const std::filesystem::path candidate = cur / "realscript.project.json";
                if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
                    foundConfig = candidate;
                    break;
                }
                const auto parent = cur.parent_path();
                if (parent == cur) break;
                cur = parent;
            }
        }
    }

    if (!foundConfig.empty()) {
        const std::string jsonText = readFile(foundConfig.string());
        std::string parseErr;
        const auto parsed = realscript::tooling::Json::parse(jsonText, parseErr);
        if (!parsed || !parsed->isObject()) {
            throw std::runtime_error(
                "invalid realscript.project.json (" + foundConfig.string() + "): " + parseErr);
        }
        (void)realscript::tooling::parseLintConfigFromProjectJson(*parsed, cfg, false);
    }
    return cfg;
}

int runLoadedBytecodeModules(
    std::vector<realscript::bytecode::Module> modules,
    const std::string& entryPoint,
    bool deterministic,
    bool profile,
    bool printDigest) {
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
        entryPoint, {}, executionOptions);
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
        return 0;
    }
    std::cerr << "runtime error "
        << realscript::runtime::errorCodeName(execution.error.code)
        << ": " << execution.error.message << '\n';
    for (const auto& frame : execution.error.stackTrace) {
        std::cerr << "  at " << frame << '\n';
    }
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }

    std::string mode = "--check";
    std::string outputPath;
    std::string formatOutputPath;
    std::string cacheDirectory;
    std::string projectConfigPath;
    realscript::optimization::Options optimizationOptions;
    optimizationOptions.level = realscript::optimization::Level::None;
    bool optimizationReport = false;
    bool deterministic = false;
    bool profile = false;
    bool printDigest = false;
    bool printCacheStats = false;
    bool formatWriteInPlace = false;
    bool formatForceStdout = false;
    bool lintRequested = false;
    bool lintDenyWarnings = false;
    std::size_t indentWidth = 4;
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
        } else if (argument == "-o" || argument == "--output") {
            if (index + 1 >= argc) {
                printUsage();
                return 2;
            }
            formatOutputPath = argv[++index];
        } else if (argument == "--project" || argument == "--config") {
            if (index + 1 >= argc) {
                printUsage();
                return 2;
            }
            projectConfigPath = argv[++index];
        } else if (argument.rfind("--project=", 0) == 0) {
            projectConfigPath = argument.substr(10);
        } else if (argument.rfind("--config=", 0) == 0) {
            projectConfigPath = argument.substr(9);
        } else if (argument == "--indent-width") {
            if (index + 1 >= argc) {
                printUsage();
                return 2;
            }
            const int parsedWidth = std::stoi(argv[++index]);
            if (parsedWidth <= 0) {
                std::cerr << "--indent-width must be >= 1\n";
                return 2;
            }
            indentWidth = static_cast<std::size_t>(parsedWidth);
        } else if (argument == "--write" || argument == "--in-place" ||
                   argument == "-i" || argument == "-w") {
            formatWriteInPlace = true;
        } else if (argument == "--stdout") {
            formatForceStdout = true;
        } else if (argument == "--format") {
            mode = "--format";
        } else if (argument == "--format-in-place") {
            mode = "--format";
            formatWriteInPlace = true;
        } else if (argument == "--check-format") {
            mode = "--check-format";
        } else if (argument == "--lint") {
            lintRequested = true;
            if (mode == "--check") {
                mode = "--lint";
            }
        } else if (argument == "--lint-deny-warnings") {
            lintDenyWarnings = true;
            lintRequested = true;
            if (mode == "--check") {
                mode = "--lint";
            }
        } else if (argument == "--cache-dir") {
            if (index + 1 >= argc) {
                printUsage();
                return 2;
            }
            cacheDirectory = argv[++index];
        } else if (argument.rfind("--cache-dir=", 0) == 0) {
            cacheDirectory = argument.substr(12);
            if (cacheDirectory.empty()) {
                printUsage();
                return 2;
            }
        } else if (argument == "--cache-stats") {
            printCacheStats = true;
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
        if (mode == "--format" || mode == "--check-format") {
            realscript::tooling::FormatOptions fmtOptions;
            fmtOptions.indentWidth = indentWidth;

            bool anyCheckFailed = false;
            for (const auto& path : paths) {
                requireScriptExtension(path);
                const std::string original = readFile(path);
                const auto res = realscript::tooling::formatSource(original, fmtOptions);
                if (res.unchangedOnError) {
                    std::cerr << path << ": error " << res.errorMessage << '\n';
                    return 1;
                }
                if (mode == "--check-format") {
                    if (res.changed) {
                        std::cerr << path << ": formatting check failed (file is not canonically formatted)\n";
                        anyCheckFailed = true;
                    }
                    continue;
                }

                // `--format`: write to `-o <path>`, write in-place (`--write`/`-i`), or print to stdout
                if (!formatOutputPath.empty()) {
                    if (paths.size() != 1) {
                        std::cerr << "-o/--output with --format accepts at most one input file\n";
                        return 2;
                    }
                    writeTextFile(formatOutputPath, res.formatted);
                } else if (formatWriteInPlace && !formatForceStdout) {
                    if (res.changed) {
                        writeTextFile(path, res.formatted);
                    }
                } else {
                    std::cout << res.formatted;
                }
            }
            return anyCheckFailed ? 1 : 0;
        }

        if (mode == "--lint") {
            std::vector<realscript::compiler::SourceFile> sources;
            sources.reserve(paths.size());
            for (const auto& path : paths) {
                requireScriptExtension(path);
                sources.push_back({path, readFile(path)});
            }

            const auto lintCfg = discoverLintConfig(projectConfigPath, paths);
            realscript::compiler::Compilation compilation(sources);
            const auto buildRes = compilation.build();
            printDiagnostics(buildRes.diagnostics);

            realscript::tooling::GovernanceLinter linter(lintCfg);
            const auto lintDiags = linter.lintWorkspace(sources, &buildRes);
            printLintDiagnostics(lintDiags);

            bool hasLintErrors = false;
            bool hasLintWarnings = false;
            for (const auto& ld : lintDiags) {
                if (ld.severity == realscript::tooling::LintSeverity::Error) {
                    hasLintErrors = true;
                } else if (ld.severity == realscript::tooling::LintSeverity::Warning) {
                    hasLintWarnings = true;
                }
            }
            const bool hasCompilerWarnings = !buildRes.diagnostics.empty();
            if (buildRes.diagnostics.hasErrors() || hasLintErrors ||
                (lintDenyWarnings && (hasLintWarnings || hasCompilerWarnings))) {
                return 1;
            }
            return 0;
        }

        if (mode == "--tokens") {
            if (paths.size() != 1) {
                std::cerr << "--tokens accepts exactly one source file\n";
                return 2;
            }
            requireScriptExtension(paths.front());

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

        if (!cacheDirectory.empty() || printCacheStats) {
            if (mode != "--check" && mode != "--run" && mode != "--bytecode" &&
                mode != "--emit-bytecode" && mode != "--emit-bytecode-dir" &&
                mode != "--mir" && mode != "--symbols") {
                printUsage();
                return 2;
            }

            std::vector<realscript::compiler::SourceFile> sources;
            sources.reserve(paths.size());
            for (const auto& path : paths) {
                requireScriptExtension(path);
                sources.push_back({path, readFile(path)});
            }

            realscript::compiler::ModuleBuildCacheOptions cacheOptions;
            cacheOptions.cacheDirectory = cacheDirectory;
            cacheOptions.optimizationOptions = optimizationOptions;
            cacheOptions.flags = realscript::compiler::kBuildCacheFlagPreserveDebugInfo |
                (deterministic ? realscript::compiler::kBuildCacheFlagDeterministic : 0u);
            cacheOptions.retainMirModules = (mode == "--mir");

            realscript::compiler::ModuleBuildCache buildCache(cacheDirectory);
            auto cached = buildCache.build(sources, cacheOptions);

            if (printCacheStats) {
                std::cerr << realscript::compiler::ModuleBuildCache::formatStatistics(
                    cached.statistics) << '\n';
            }

            if (!cached.diagnostics.hasErrors() && optimizationReport) {
                if (optimizationOptions.level != realscript::optimization::Level::None) {
                    std::cerr << realscript::optimization::levelName(
                        optimizationOptions.level) << ' '
                        << realscript::optimization::formatStatistics(
                            cached.optimizationStatistics) << '\n';
                } else {
                    std::cerr << "O0 functions=0 iterations=0 constants-folded=0 "
                        "local-loads-folded=0 branches-folded=0 "
                        "instructions-removed=0 blocks-removed=0\n";
                }
            }

            if (!cached.diagnostics.hasErrors()) {
                if (mode == "--run") {
                    const int runStatus = runLoadedBytecodeModules(
                        std::move(cached.bytecodeModules),
                        outputPath,
                        deterministic,
                        profile,
                        printDigest);
                    if (runStatus != 0) return runStatus;
                } else if (mode == "--bytecode") {
                    for (const auto& module : cached.bytecodeModules) {
                        std::cout << realscript::bytecode::disassembleModule(module)
                            << '\n';
                    }
                } else if (mode == "--emit-bytecode") {
                    if (cached.bytecodeModules.size() != 1) {
                        cached.diagnostics.report(
                            "RS5200",
                            "--emit-bytecode requires exactly one compiled module",
                            {});
                    } else {
                        writeBinaryFile(
                            outputPath,
                            realscript::bytecode::encodeModule(
                                cached.bytecodeModules.front()));
                    }
                } else if (mode == "--emit-bytecode-dir") {
                    std::error_code directoryError;
                    std::filesystem::create_directories(
                        outputPath, directoryError);
                    if (directoryError) {
                        cached.diagnostics.report(
                            "RS5201",
                            "failed to create bytecode output directory: " +
                                directoryError.message(),
                            {});
                    } else {
                        for (const auto& module : cached.bytecodeModules) {
                            const auto artifact =
                                std::filesystem::path(outputPath) /
                                bytecodeArtifactName(module.name);
                            writeBinaryFile(
                                artifact.string(),
                                realscript::bytecode::encodeModule(module));
                        }
                    }
                } else if (mode == "--mir") {
                    for (const auto& module : cached.mirModules) {
                        std::cout << realscript::mir::printModule(module);
                    }
                } else if (mode == "--symbols") {
                    for (const auto& module : cached.bytecodeModules) {
                        for (const auto& function : module.functions) {
                            std::cout << module.name << "::" << function.name
                                << " 0x" << std::hex << function.symbolId
                                << std::dec << '\n';
                        }
                    }
                }
            }

            printDiagnostics(cached.diagnostics);
            return cached.diagnostics.hasErrors() ? 1 : 0;
        }

        std::vector<realscript::compiler::SourceFile> loadedSources;
        loadedSources.reserve(paths.size());
        realscript::compiler::Compilation compilation;
        for (const auto& path : paths) {
            requireScriptExtension(path);
            auto text = readFile(path);
            loadedSources.push_back({path, text});
            compilation.addSource({path, std::move(text)});
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
                const int runStatus = runLoadedBytecodeModules(
                    std::move(modules),
                    outputPath,
                    deterministic,
                    profile,
                    printDigest);
                if (runStatus != 0) return runStatus;
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
        if (lintRequested) {
            const auto lintCfg = discoverLintConfig(projectConfigPath, paths);
            realscript::tooling::GovernanceLinter linter(lintCfg);
            const auto lintDiags = linter.lintWorkspace(loadedSources, &result);
            printLintDiagnostics(lintDiags);
            for (const auto& ld : lintDiags) {
                if (ld.severity == realscript::tooling::LintSeverity::Error ||
                    (lintDenyWarnings && ld.severity == realscript::tooling::LintSeverity::Warning)) {
                    return 1;
                }
            }
        }
        return result.diagnostics.hasErrors() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "rsc: " << error.what() << '\n';
        return 2;
    }
}
