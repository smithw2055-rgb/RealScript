#include "realscript/jit/Jit.h"

#include "realscript/aot_cpp/RuntimeAbi.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace realscript::jit {
namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

bool writeFileIfChanged(
    const std::filesystem::path& path,
    const std::string& content) {
    if (readFile(path) == content) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(stream);
}

std::string quote(const std::filesystem::path& path) {
#if defined(_WIN32)
    auto value = path.string();
    std::string result = "\"";
    for (const auto character : value) {
        if (character == '"') result += '\\';
        result += character;
    }
    result += '"';
    return result;
#else
    const auto value = path.string();
    std::string result = "'";
    for (const auto character : value) {
        if (character == '\'') result += "'\\''";
        else result += character;
    }
    result += '\'';
    return result;
#endif
}

std::string quoteArgument(const std::string& argument) {
    return quote(std::filesystem::path(argument));
}

std::string hexHash(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}


void mixHash(std::uint64_t& hash, std::string_view value) noexcept {
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= prime;
    }
    hash ^= 0xffU;
    hash *= prime;
}

std::uint64_t cacheFingerprint(
    std::uint64_t contentHash,
    const ToolchainOptions& options) {
    std::uint64_t hash = 1469598103934665603ULL;
    mixHash(hash, std::to_string(contentHash));
    mixHash(hash, options.compiler.string());
    mixHash(hash, options.includeDirectory.string());
    mixHash(hash, options.supportLibrary.string());
    mixHash(hash, std::to_string(aot::RuntimeAbiMajor));
    mixHash(hash, std::to_string(aot::RuntimeAbiMinor));
    for (const auto& argument : options.compilerArguments) mixHash(hash, argument);

    std::error_code error;
    const auto size = std::filesystem::file_size(options.supportLibrary, error);
    if (!error) mixHash(hash, std::to_string(size));
    error.clear();
    const auto modified = std::filesystem::last_write_time(
        options.supportLibrary, error);
    if (!error) {
        mixHash(hash, std::to_string(modified.time_since_epoch().count()));
    }
    return hash;
}

std::string libraryExtension() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

bool looksLikeMsvc(const std::filesystem::path& compiler) {
    auto name = compiler.filename().string();
    for (auto& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return name == "cl" || name == "cl.exe";
}

std::string compileCommand(
    const ToolchainOptions& options,
    const std::filesystem::path& source,
    const std::filesystem::path& library,
    const std::filesystem::path& log) {
    std::ostringstream command;
    command << quote(options.compiler) << ' ';
    if (looksLikeMsvc(options.compiler)) {
        command << "/nologo /std:c++17 /EHsc /LD "
            "/DREALSCRIPT_AOT_BUILD_SHARED_MODULE=1 ";
        command << "/I" << quote(options.includeDirectory) << ' ';
        for (const auto& argument : options.compilerArguments) {
            command << quoteArgument(argument) << ' ';
        }
        command << quote(source) << ' ' << quote(options.supportLibrary)
            << " /Fe:" << quote(library);
    } else {
        command << "-std=c++17 -shared -fPIC -pthread "
            "-DREALSCRIPT_AOT_BUILD_SHARED_MODULE=1 ";
        command << "-I" << quote(options.includeDirectory) << ' ';
        for (const auto& argument : options.compilerArguments) {
            command << quoteArgument(argument) << ' ';
        }
        command << quote(source) << ' ' << quote(options.supportLibrary)
            << " -o " << quote(library);
    }
    command << " > " << quote(log) << " 2>&1";
    return command.str();
}

#if defined(_WIN32)
using LibraryHandle = HMODULE;
using LibrarySymbol = FARPROC;
void closeLibrary(LibraryHandle handle) {
    if (handle) FreeLibrary(handle);
}
LibrarySymbol findSymbol(LibraryHandle handle, const std::string& name) {
    return GetProcAddress(handle, name.c_str());
}
std::string lastLibraryError() {
    return "LoadLibrary/GetProcAddress failed with code " +
        std::to_string(static_cast<unsigned long>(GetLastError()));
}
LibraryHandle openLibrary(const std::filesystem::path& path) {
    return LoadLibraryW(path.wstring().c_str());
}
#else
using LibraryHandle = void*;
using LibrarySymbol = void*;
void closeLibrary(LibraryHandle handle) {
    if (handle) dlclose(handle);
}
LibrarySymbol findSymbol(LibraryHandle handle, const std::string& name) {
    return dlsym(handle, name.c_str());
}
std::string lastLibraryError() {
    const auto* error = dlerror();
    return error ? error : "dynamic loader failed";
}
LibraryHandle openLibrary(const std::filesystem::path& path) {
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}
#endif

} // namespace

struct Module::Impl {
    LibraryHandle handle = nullptr;
    std::unique_ptr<aot::Program> program;
    const aot::ProgramDescriptor* descriptor = nullptr;
    std::filesystem::path path;
    std::uint64_t hash = 0;

    ~Impl() {
        program.reset();
        closeLibrary(handle);
    }
};

Module::Module() = default;
Module::Module(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Module::~Module() = default;
Module::Module(Module&&) noexcept = default;
Module& Module::operator=(Module&&) noexcept = default;

bool Module::valid() const noexcept {
    return impl_ && impl_->handle && impl_->program && impl_->descriptor;
}

std::uint64_t Module::contentHash() const noexcept {
    return impl_ ? impl_->hash : 0;
}

const std::filesystem::path& Module::libraryPath() const noexcept {
    static const std::filesystem::path empty;
    return impl_ ? impl_->path : empty;
}

const aot::ProgramDescriptor* Module::descriptor() const noexcept {
    return impl_ ? impl_->descriptor : nullptr;
}

void Module::setBindings(std::shared_ptr<const runtime::BindingRegistry> bindings) {
    if (valid()) impl_->program->setBindings(std::move(bindings));
}

void Module::setHeap(std::shared_ptr<runtime::ManagedHeap> heap) {
    if (valid()) impl_->program->setHeap(std::move(heap));
}

runtime::ExecutionResult Module::invoke(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult result;
        result.error.code = runtime::ErrorCode::InvalidProgram;
        result.error.message = "JIT module is not loaded";
        return result;
    }
    return impl_->program->invoke(
        qualifiedName,
        arguments,
        std::move(options));
}

bool ToolchainJit::available(const ToolchainOptions& options) noexcept {
    std::error_code error;
    return !options.compiler.empty() &&
        std::filesystem::exists(options.compiler, error) &&
        !options.includeDirectory.empty() &&
        std::filesystem::exists(options.includeDirectory, error) &&
        !options.supportLibrary.empty() &&
        std::filesystem::exists(options.supportLibrary, error);
}

CompileResult ToolchainJit::compile(
    std::vector<mir::Module> modules,
    ToolchainOptions options) const {
    CompileResult result;
    if (!available(options)) {
        result.error = "C++17 JIT toolchain paths are incomplete or unavailable";
        return result;
    }
    if (options.outputDirectory.empty()) {
        result.error = "C++17 JIT output directory is empty";
        return result;
    }

    diagnostics::DiagnosticBag diagnostics;
    optimization::Optimizer optimizer;
    auto optimized = optimizer.optimize(
        std::move(modules),
        diagnostics,
        options.optimization);
    result.optimizationStatistics = optimized.statistics;
    if (diagnostics.hasErrors()) {
        result.error = "MIR optimization failed before JIT compilation";
        return result;
    }

    aot::CppGenerator generator;
    auto generated = generator.generate(
        optimized.modules,
        diagnostics,
        options.generation);
    if (diagnostics.hasErrors() || generated.source.empty()) {
        result.error = "C++17 generation failed before JIT compilation";
        return result;
    }

    const auto semanticHash = hexHash(generated.contentHash);
    const auto cacheHash = hexHash(cacheFingerprint(generated.contentHash, options));
    const auto directory = options.outputDirectory / cacheHash;
    const auto header = directory / "realscript_aot_generated.h";
    const auto source = directory / "realscript_aot_generated.cpp";
    const auto manifest = directory / "realscript_aot_manifest.json";
    const auto library = directory /
        (std::string("realscript_jit_") + semanticHash + libraryExtension());
    const auto log = directory / "compile.log";
    std::filesystem::create_directories(directory);
    (void)writeFileIfChanged(header, generated.header);
    (void)writeFileIfChanged(source, generated.source);
    (void)writeFileIfChanged(manifest, generated.manifest);

    std::error_code fileError;
    if (!options.reuseCachedLibrary ||
        !std::filesystem::exists(library, fileError)) {
        const auto command = compileCommand(options, source, library, log);
        const auto status = std::system(command.c_str());
        result.compilerOutput = readFile(log);
        if (status != 0 || !std::filesystem::exists(library, fileError)) {
            result.error = "C++17 JIT compiler failed";
            if (!result.compilerOutput.empty()) {
                result.error += ":\n" + result.compilerOutput;
            }
            return result;
        }
    } else {
        result.cacheHit = true;
        result.compilerOutput = readFile(log);
    }

    auto impl = std::make_unique<Module::Impl>();
    impl->handle = openLibrary(library);
    if (!impl->handle) {
        result.error = "cannot load JIT library '" + library.string() +
            "': " + lastLibraryError();
        return result;
    }
    const auto queryName = options.generation.querySymbol.empty()
        ? std::string("rs_module_query_v1")
        : options.generation.querySymbol;
    const auto symbol = findSymbol(impl->handle, queryName);
    if (!symbol) {
        result.error = "JIT library does not export '" + queryName +
            "': " + lastLibraryError();
        return result;
    }
    using Query = RsStatusV1 (*)(const RsRuntimeApiV1*, RsModuleExportsV1*);
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4191)
#endif
    const auto query = reinterpret_cast<Query>(symbol);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    RsRuntimeApiV1 api{
        sizeof(RsRuntimeApiV1),
        aot::RuntimeAbiMajor,
        aot::RuntimeAbiMinor,
        nullptr,
    };
    RsModuleExportsV1 exports{};
    exports.size = sizeof(exports);
    if (query(&api, &exports) != RS_STATUS_V1_OK ||
        !exports.program_descriptor) {
        result.error = "JIT module query rejected the current runtime ABI";
        return result;
    }
    impl->descriptor = static_cast<const aot::ProgramDescriptor*>(
        exports.program_descriptor);
    try {
        impl->program = std::make_unique<aot::Program>(*impl->descriptor);
    } catch (const std::exception& exception) {
        result.error = std::string("JIT descriptor validation failed: ") +
            exception.what();
        return result;
    }
    impl->path = library;
    impl->hash = generated.contentHash;
    result.module = std::shared_ptr<Module>(new Module(std::move(impl)));

    if (!options.keepGeneratedSources) {
        std::filesystem::remove(header, fileError);
        std::filesystem::remove(source, fileError);
        std::filesystem::remove(manifest, fileError);
    }
    return result;
}

} // namespace realscript::jit
