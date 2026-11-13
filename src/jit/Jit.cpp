#include "realscript/jit/Jit.h"

#include "../runtime/ExecutionMirror.h"
#include "realscript/aot_cpp/RuntimeAbi.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/runtime/AsyncRuntime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif

namespace realscript::jit {
namespace {

// ============================================================================
// ToolchainJit Helpers (100% Backward Compatible)
// ============================================================================

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
            << " /Fe:" << quote(library)
            << " /Fo:" << quote(
                library.parent_path() / "realscript_aot_generated.obj");
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
#if defined(_WIN32)
    if (looksLikeMsvc(options.compiler)) {
        if (GetEnvironmentVariableA("INCLUDE", nullptr, 0) == 0) {
            auto dir = options.compiler.parent_path();
            for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
                const auto vcvars = dir / "Auxiliary" / "Build" / "vcvars64.bat";
                std::error_code ec;
                if (std::filesystem::exists(vcvars, ec)) {
                    return "cmd.exe /D /S /C \"call " + quote(vcvars) +
                        " >nul 2>&1 && " + command.str() + "\"";
                }
                const auto parent = dir.parent_path();
                if (parent == dir) break;
                dir = parent;
            }
        }
    }
    return "cmd.exe /D /S /C \"" + command.str() + "\"";
#else
    return command.str();
#endif
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
        const_cast<void*>(static_cast<const void*>(runtime::detail::hostValuePoolBridge())),
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

// ============================================================================
// P2-A: ExecutableCodeArena (Strict W^X Page Manager)
// ============================================================================

namespace {

template <typename FnPtr, typename Ptr>
[[nodiscard]] FnPtr bitCastFnPtr(Ptr ptr) noexcept {
    static_assert(sizeof(FnPtr) == sizeof(Ptr), "function pointer size mismatch");
    FnPtr out = nullptr;
    std::memcpy(&out, &ptr, sizeof(out));
    return out;
}

template <typename FnPtr>
[[nodiscard]] std::uint64_t fnPtrAddress(FnPtr fn) noexcept {
    static_assert(sizeof(FnPtr) == sizeof(std::uintptr_t), "function pointer size mismatch");
    std::uintptr_t addr = 0;
    std::memcpy(&addr, &fn, sizeof(addr));
    return static_cast<std::uint64_t>(addr);
}

[[nodiscard]] std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept {
    if (alignment <= 1) return value;
    const std::size_t rem = value % alignment;
    return rem == 0 ? value : (value + (alignment - rem));
}

struct ScopedJitWriteEnable {
    bool active = false;
    explicit ScopedJitWriteEnable(bool isMapJit) noexcept : active(isMapJit) {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        if (active) {
            pthread_jit_write_protect_np(0);
        }
#endif
    }
    ~ScopedJitWriteEnable() noexcept {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        if (active) {
            pthread_jit_write_protect_np(1);
        }
#endif
    }
    ScopedJitWriteEnable(const ScopedJitWriteEnable&) = delete;
    ScopedJitWriteEnable& operator=(const ScopedJitWriteEnable&) = delete;
};

void fillArchTrapPadding(std::uint8_t* dst, std::size_t byteLen) noexcept {
    if (!dst || byteLen == 0) return;
#if defined(__aarch64__) || defined(_M_ARM64)
    // ARM64: fill 4-byte aligned slots with `brk #0` (0xD4200000 little-endian)
    constexpr std::uint32_t kArm64Brk0 = 0xD4200000u;
    std::size_t off = 0;
    while (off + 4u <= byteLen) {
        std::memcpy(dst + off, &kArm64Brk0, 4u);
        off += 4u;
    }
    if (off < byteLen) {
        std::memset(dst + off, 0, byteLen - off);
    }
#else
    std::memset(dst, 0xCC, byteLen);
#endif
}

[[nodiscard]] std::uint8_t* osAllocateCodePages(std::size_t byteSize, bool& outMapJit) noexcept {
    outMapJit = false;
    if (byteSize == 0) return nullptr;
#if defined(_WIN32)
    void* mem = VirtualAlloc(nullptr, byteSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return static_cast<std::uint8_t*>(mem);
#else
#if defined(MAP_ANONYMOUS)
    constexpr int kAnonFlag = MAP_ANONYMOUS;
#elif defined(MAP_ANON)
    constexpr int kAnonFlag = MAP_ANON;
#else
    constexpr int kAnonFlag = 0;
#endif
    if (kAnonFlag == 0) return nullptr;
#if defined(__APPLE__) && defined(TARGET_OS_OSX) && TARGET_OS_OSX && (defined(__aarch64__) || defined(__arm64__)) && defined(MAP_JIT)
    // macOS Apple Silicon: prefer MAP_JIT + pthread_jit_write_protect_np hardware APRR W^X
    void* jitMem = mmap(
        nullptr,
        byteSize,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | kAnonFlag | MAP_JIT,
        -1,
        0);
    if (jitMem != MAP_FAILED) {
        outMapJit = true;
        pthread_jit_write_protect_np(1);
        return static_cast<std::uint8_t*>(jitMem);
    }
#endif
    void* mem = mmap(nullptr, byteSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | kAnonFlag, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    return static_cast<std::uint8_t*>(mem);
#endif
}

[[nodiscard]] bool osProtectRxPages(std::uint8_t* base, std::size_t byteSize, bool isMapJit) noexcept {
    if (!base || byteSize == 0) return false;
#if defined(_WIN32)
    (void)isMapJit;
    DWORD oldProtect = 0;
    if (VirtualProtect(base, byteSize, PAGE_EXECUTE_READ, &oldProtect) == 0) {
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), base, byteSize);
    return true;
#else
    if (!isMapJit) {
        if (mprotect(base, byteSize, PROT_READ | PROT_EXEC) != 0) {
            return false;
        }
    }
#if defined(__APPLE__)
    sys_icache_invalidate(base, byteSize);
#elif defined(__GNUC__) || defined(__clang__)
    char* begin = reinterpret_cast<char*>(base);
    __builtin___clear_cache(begin, begin + byteSize);
#endif
    return true;
#endif
}

void osFreePages(std::uint8_t* base, std::size_t byteSize) noexcept {
    if (!base || byteSize == 0) return;
#if defined(_WIN32)
    (void)byteSize;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, byteSize);
#endif
}

} // namespace

ExecutableCodeArena::ExecutableCodeArena(std::size_t defaultBlockBytes)
    : defaultBlockBytes_(defaultBlockBytes == 0 ? 65536 : defaultBlockBytes) {}

ExecutableCodeArena::~ExecutableCodeArena() {
    releaseAll();
}

ExecutableCodeArena::ExecutableCodeArena(ExecutableCodeArena&& other) noexcept
    : blocks_(std::move(other.blocks_)),
      defaultBlockBytes_(other.defaultBlockBytes_),
      allocatedBytes_(other.allocatedBytes_),
      mappedBytes_(other.mappedBytes_),
      sealed_(other.sealed_) {
    other.allocatedBytes_ = 0;
    other.mappedBytes_ = 0;
    other.sealed_ = false;
}

ExecutableCodeArena& ExecutableCodeArena::operator=(ExecutableCodeArena&& other) noexcept {
    if (this != &other) {
        releaseAll();
        blocks_ = std::move(other.blocks_);
        defaultBlockBytes_ = other.defaultBlockBytes_;
        allocatedBytes_ = other.allocatedBytes_;
        mappedBytes_ = other.mappedBytes_;
        sealed_ = other.sealed_;
        other.allocatedBytes_ = 0;
        other.mappedBytes_ = 0;
        other.sealed_ = false;
    }
    return *this;
}

void ExecutableCodeArena::releaseAll() noexcept {
    for (auto& blk : blocks_) {
        osFreePages(blk.base, blk.capacity);
        blk.base = nullptr;
        blk.capacity = 0;
        blk.used = 0;
        blk.executable = false;
    }
    blocks_.clear();
    allocatedBytes_ = 0;
    mappedBytes_ = 0;
    sealed_ = false;
}

std::size_t ExecutableCodeArena::systemPageSize() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize > 0 ? static_cast<std::size_t>(info.dwPageSize) : 4096u;
#else
    const long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<std::size_t>(ps) : 4096u;
#endif
}

std::uint8_t* ExecutableCodeArena::emit(
    const std::uint8_t* code,
    std::size_t size,
    std::size_t alignment) {
    if (sealed_ || !code || size == 0) return nullptr;
#if defined(__aarch64__) || defined(_M_ARM64)
    const std::size_t align = std::max<std::size_t>(alignUp(alignment, 4u), 4u);
#else
    const std::size_t align = std::max<std::size_t>(alignment, 1);
#endif

    if (!blocks_.empty() && !blocks_.back().executable) {
        auto& cur = blocks_.back();
        const std::size_t alignedOff = alignUp(cur.used, align);
        if (alignedOff <= cur.capacity && size <= cur.capacity - alignedOff) {
            ScopedJitWriteEnable writeGuard(cur.mapJit);
            if (alignedOff > cur.used) {
                fillArchTrapPadding(cur.base + cur.used, alignedOff - cur.used);
            }
            std::uint8_t* dst = cur.base + alignedOff;
            std::memcpy(dst, code, size);
            cur.used = alignedOff + size;
            allocatedBytes_ += size;
            return dst;
        }
    }

    const std::size_t pageSz = systemPageSize();
    const std::size_t allocCap = alignUp(std::max(defaultBlockBytes_, size + align), pageSz);
    bool isMapJit = false;
    std::uint8_t* base = osAllocateCodePages(allocCap, isMapJit);
    if (!base) return nullptr;

    Block blk{};
    blk.base = base;
    blk.capacity = allocCap;
    blk.used = size;
    blk.executable = false;
    blk.mapJit = isMapJit;
    {
        ScopedJitWriteEnable writeGuard(blk.mapJit);
        std::memcpy(blk.base, code, size);
    }
    blocks_.push_back(blk);
    mappedBytes_ += allocCap;
    allocatedBytes_ += size;
    return base;
}

bool ExecutableCodeArena::seal() noexcept {
    if (sealed_) return true;
    for (auto& blk : blocks_) {
        if (blk.executable) continue;
        if (blk.used < blk.capacity) {
            ScopedJitWriteEnable writeGuard(blk.mapJit);
            fillArchTrapPadding(blk.base + blk.used, blk.capacity - blk.used);
        }
        if (!osProtectRxPages(blk.base, blk.capacity, blk.mapJit)) {
            return false;
        }
        blk.executable = true;
    }
    sealed_ = true;
    return true;
}

bool ExecutableCodeArena::sealed() const noexcept { return sealed_; }
std::size_t ExecutableCodeArena::allocatedBytes() const noexcept { return allocatedBytes_; }
std::size_t ExecutableCodeArena::mappedBytes() const noexcept { return mappedBytes_; }
std::size_t ExecutableCodeArena::blockCount() const noexcept { return blocks_.size(); }

bool ExecutableCodeArena::nativeCodeExecutionSupported() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    static const bool supported = []() noexcept {
        ExecutableCodeArena probe(systemPageSize());
        // mov eax, 0x52534a54; ret
        static constexpr std::uint8_t kProbeCode[] = {
            0xB8, 0x54, 0x4A, 0x53, 0x52, 0xC3
        };
        std::uint8_t* entry = probe.emit(kProbeCode, sizeof(kProbeCode), 16);
        if (!entry || !probe.seal()) return false;
        using ProbeFn = std::uint32_t (*)();
        const auto fn = bitCastFnPtr<ProbeFn>(entry);
        return fn() == 0x52534A54u;
    }();
    return supported;
#elif defined(__aarch64__) || defined(_M_ARM64)
    static const bool supported = []() noexcept {
        ExecutableCodeArena probe(systemPageSize());
        // movz w0, #0x4a54; movk w0, #0x5253, lsl #16; ret
        static constexpr std::uint32_t kProbeWords[] = {
            0x52894A80u,
            0x72AA4A60u,
            0xD65F03C0u,
        };
        std::uint8_t* entry = probe.emit(
            reinterpret_cast<const std::uint8_t*>(kProbeWords),
            sizeof(kProbeWords),
            16);
        if (!entry || !probe.seal()) return false;
        using ProbeFn = std::uint32_t (*)();
        const auto fn = bitCastFnPtr<ProbeFn>(entry);
        return fn() == 0x52534A54u;
    }();
    return supported;
#else
    return false;
#endif
}

// ============================================================================
// P2-A: Value Layout Static Verification & Runtime Execution ABI
// ============================================================================

namespace {

// Compile-time verification of Value 16-byte layout contract required by StencilJit
struct ValueLayoutProbe {
    static bool verify() noexcept {
        runtime::Value iv(std::int64_t{-0x12345678LL});
        runtime::Value dv(3.5);
        runtime::Value bv(true);
        std::int64_t iPayload = 0;
        double dPayload = 0.0;
        std::uint8_t bPayload = 0;
        std::uint8_t iTag = 0, dTag = 0, bTag = 0;
        std::memcpy(&iPayload, &iv, 8);
        std::memcpy(&iTag, reinterpret_cast<const std::uint8_t*>(&iv) + 15, 1);
        std::memcpy(&dPayload, &dv, 8);
        std::memcpy(&dTag, reinterpret_cast<const std::uint8_t*>(&dv) + 15, 1);
        std::memcpy(&bPayload, &bv, 1);
        std::memcpy(&bTag, reinterpret_cast<const std::uint8_t*>(&bv) + 15, 1);
        return iPayload == -0x12345678LL &&
               iTag == static_cast<std::uint8_t>(runtime::ValueTag::Int64) &&
               dPayload == 3.5 &&
               dTag == static_cast<std::uint8_t>(runtime::ValueTag::Double) &&
               bPayload == 1u &&
               bTag == static_cast<std::uint8_t>(runtime::ValueTag::Bool);
    }
};
static_assert(sizeof(runtime::Value) == 16, "StencilJit requires 16-byte Value");
static_assert(alignof(runtime::Value) == 16, "StencilJit requires 16-byte aligned Value");
static_assert(sizeof(runtime::ObjectRef) == 16, "StencilJit requires 16-byte ObjectRef");

constexpr std::uint8_t kTagBool = static_cast<std::uint8_t>(runtime::ValueTag::Bool);       // 4
constexpr std::uint8_t kTagInt64 = static_cast<std::uint8_t>(runtime::ValueTag::Int64);     // 9
constexpr std::uint8_t kTagLong = static_cast<std::uint8_t>(runtime::ValueTag::Long);       // 11
constexpr std::uint8_t kTagFloat = static_cast<std::uint8_t>(runtime::ValueTag::Float);     // 13
constexpr std::uint8_t kTagDouble = static_cast<std::uint8_t>(runtime::ValueTag::Double);   // 14
constexpr std::uint8_t kTagObjectRef = static_cast<std::uint8_t>(runtime::ValueTag::ObjectRef); // 27

constexpr std::uint8_t kPackedBool = static_cast<std::uint8_t>(runtime::PackedElemEncoding::Bool);       // 1
constexpr std::uint8_t kPackedInt32 = static_cast<std::uint8_t>(runtime::PackedElemEncoding::Int32);     // 7
constexpr std::uint8_t kPackedFloat64 = static_cast<std::uint8_t>(runtime::PackedElemEncoding::Float64); // 13

[[nodiscard]] constexpr bool fitsInt32(std::int64_t v) noexcept {
    return v >= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) &&
           v <= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
}

[[nodiscard]] constexpr std::int64_t wrapInt32Val(std::int64_t v) noexcept {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(v)));
}

struct alignas(16) JitArrayCacheEntry {
    std::uint64_t slotAndGen;        // offset 0  (0xffffffffffffffffULL when invalid)
    std::uint32_t heapId;            // offset 8
    std::uint8_t kind;               // offset 12
    std::uint8_t isPacked;           // offset 13 (1 = Packed, 2 = FlatStruct, 0 = Boxed)
    std::uint8_t encoding;           // offset 14 (PackedElemEncoding)
    std::uint8_t elementType;        // offset 15 (semantic::PrimitiveType)
    void* rawData;                   // offset 16 (packed rawData, FlatStruct Value*, or boxed Value*)
    std::uint32_t length;            // offset 24
    std::uint16_t flatStride;        // offset 28 (FlatStruct[] leaf stride)
    std::uint16_t flatSchemaId;      // offset 30 (FlatStruct[] schemaId)
    std::uint64_t elementTypeId;     // offset 32
    std::uint8_t* dirtyCards;        // offset 40 (FlatStruct[] GC card table or nullptr)
    std::uint32_t blittableSchemaId; // offset 48 (Packed BlittableStruct schemaId)
    std::uint8_t flatFlags;          // offset 52 (bit0=hasRefs, bit1=hasPool, bit2=needsRemBarrier)
    std::uint8_t padByte0;           // offset 53
    std::uint16_t padWord1;          // offset 54
    std::uint64_t pad3;              // offset 56
};
static_assert(std::is_standard_layout_v<JitArrayCacheEntry>, "JitArrayCacheEntry must be standard layout");
static_assert(sizeof(JitArrayCacheEntry) == 64, "JitArrayCacheEntry must be 64 bytes");
static_assert(offsetof(JitArrayCacheEntry, slotAndGen) == 0);
static_assert(offsetof(JitArrayCacheEntry, heapId) == 8);
static_assert(offsetof(JitArrayCacheEntry, kind) == 12);
static_assert(offsetof(JitArrayCacheEntry, isPacked) == 13);
static_assert(offsetof(JitArrayCacheEntry, encoding) == 14);
static_assert(offsetof(JitArrayCacheEntry, elementType) == 15);
static_assert(offsetof(JitArrayCacheEntry, rawData) == 16);
static_assert(offsetof(JitArrayCacheEntry, length) == 24);
static_assert(offsetof(JitArrayCacheEntry, flatStride) == 28);
static_assert(offsetof(JitArrayCacheEntry, flatSchemaId) == 30);
static_assert(offsetof(JitArrayCacheEntry, elementTypeId) == 32);
static_assert(offsetof(JitArrayCacheEntry, dirtyCards) == 40);
static_assert(offsetof(JitArrayCacheEntry, blittableSchemaId) == 48);
static_assert(offsetof(JitArrayCacheEntry, flatFlags) == 52);

struct JitRuntimeSession;
struct CompiledFunctionRecord;

struct alignas(16) JitAbiContext {
    runtime::Value* frameBase;                 // offset 0
    std::uint64_t remainingBudget;             // offset 8
    std::uint64_t branchesTaken;               // offset 16
    runtime::Value* returnSlot;                // offset 24
    std::uint32_t errorFlatIndex;              // offset 32
    std::uint32_t errorCodeRaw;                // offset 36
    JitRuntimeSession* session;                // offset 40
    const CompiledFunctionRecord* compiledFn;  // offset 48
    std::uint32_t branchCondOut;               // offset 56 (used by slow array-length branch helper)
    std::uint32_t nextFlatOffsetOut;           // offset 60 (used by InlineCallEnter / slow helpers)
    JitArrayCacheEntry arrayCache[4];          // offset 64 .. 319
};
static_assert(std::is_standard_layout_v<JitAbiContext>, "JitAbiContext must be standard layout");
static_assert(offsetof(JitAbiContext, frameBase) == 0);
static_assert(offsetof(JitAbiContext, remainingBudget) == 8);
static_assert(offsetof(JitAbiContext, branchesTaken) == 16);
static_assert(offsetof(JitAbiContext, returnSlot) == 24);
static_assert(offsetof(JitAbiContext, errorFlatIndex) == 32);
static_assert(offsetof(JitAbiContext, errorCodeRaw) == 36);
static_assert(offsetof(JitAbiContext, session) == 40);
static_assert(offsetof(JitAbiContext, compiledFn) == 48);
static_assert(offsetof(JitAbiContext, branchCondOut) == 56);
static_assert(offsetof(JitAbiContext, nextFlatOffsetOut) == 60);
static_assert(offsetof(JitAbiContext, arrayCache) == 64);

using NativeJitEntryFn = std::uint32_t (*)(JitAbiContext* ctx);

inline void invalidateJitArrayCaches(JitAbiContext& ctx) noexcept {
    for (auto& ce : ctx.arrayCache) {
        ce.slotAndGen = 0xffffffffffffffffULL;
        ce.heapId = 0;
        ce.kind = 0;
        ce.isPacked = 0;
        ce.encoding = 0;
        ce.elementType = 0;
        ce.rawData = nullptr;
        ce.length = 0;
        ce.flatStride = 0;
        ce.flatSchemaId = 0;
        ce.elementTypeId = 0;
        ce.dirtyCards = nullptr;
        ce.blittableSchemaId = 0;
        ce.flatFlags = 0;
    }
}

struct JitRuntimeSession {
    const runtime::ProgramImage* program = nullptr;
    runtime::ManagedHeap* heap = nullptr;
    runtime::Limits limits{};
    runtime::RuntimeStatistics stats{};
    runtime::RuntimeError error{};
    std::vector<runtime::Value>* values = nullptr;
    std::size_t frameOffset = 0;
    std::size_t valueTop = 0;
    std::size_t callDepth = 0;
    runtime::Value* valuesBase = nullptr;
    std::uint32_t frameOffset32 = 0;
    std::uint8_t flatBankId = 0;
    runtime::ShadowStack shadowStack{};
};

struct StencilTlsScratch {
    std::vector<runtime::Value> values;
    bool inUse = false;
};
thread_local StencilTlsScratch g_stencilTlsScratch;

// ============================================================================
// Direct-Threaded C++ Stencil Thunk Representation (Cross-Platform Fast Tier)
// ============================================================================

enum class StencilStepStatus : std::uint8_t {
    Continue = 0,
    ReturnOk = 1,
    Error = 2,
};

struct ThreadedStencilOp;
using ThreadedStencilFn = StencilStepStatus (*)(
    JitAbiContext* ctx,
    const ThreadedStencilOp*& pc) noexcept;

struct ThreadedStencilOp {
    ThreadedStencilFn handler = nullptr;
    const ThreadedStencilOp* trueTarget = nullptr;
    const ThreadedStencilOp* falseTarget = nullptr;
    std::int64_t imm64 = 0;
    std::uint32_t dstOff = 0;
    std::uint32_t src0Off = 0;
    std::uint32_t src1Off = 0;
    std::uint32_t src2Off = 0;
    std::uint32_t aux0Off = 0;
    std::uint32_t aux1Off = 0;
    std::uint32_t storeOff = 0;
    std::uint32_t flatIdx = 0;
    std::uint8_t span = 1;
    std::uint8_t unexecutedOnTrap = 0;
    std::uint8_t flags = 0;
    std::uint8_t auxByte = 0;
};

struct CompiledFunctionRecord {
    semantic::SymbolId symbolId = 0;
    const runtime::ProgramImage::FunctionLocation* location = nullptr;
    const runtime::detail::ExecutionFunction* execFunc = nullptr;
    std::atomic<StencilTier> tier{StencilTier::InterpreterFallback};
    std::atomic<NativeJitEntryFn> nativeEntry{nullptr};
    std::uint8_t* rawCodePtr = nullptr;
    std::size_t rawCodeSize = 0;
    bool eligibleForNative = false;
    bool eligibleForThreaded = false;
    mutable std::atomic<std::uint32_t> callCount{0};
    std::vector<std::uint8_t> unexecutedAfterOp;
    std::vector<ThreadedStencilOp> threadedStream;
};

// ============================================================================
// Shared Runtime Error, Array Cache & Slow-Path C++ Helpers
// ============================================================================

void populateJitErrorDiagnostics(
    JitRuntimeSession& session,
    const runtime::ProgramImage::FunctionLocation& location,
    const runtime::detail::ExecutionFunction& execFunc,
    std::uint32_t flatIdx,
    runtime::ErrorCode code,
    std::string message) {
    session.error.code = code;
    session.error.message = std::move(message);
    session.error.stackTrace.clear();
    session.error.sourceLocations.clear();
    session.error.symbolizedStackTrace.clear();

    const runtime::ProgramImage::FunctionLocation* inlinedLoc = nullptr;
    const runtime::detail::ExecutionFunction* inlinedExec = nullptr;
    std::uint32_t callerBlkIdx = 0;
    std::uint32_t callerInstIdx = 0;
    std::uint32_t activeBlkIdx = 0;
    std::uint32_t activeInstIdx = 0;
    std::uint32_t activeSpan = 1;
    bool isTerm = false;

    if (flatIdx < execFunc.flatStream.size() && flatIdx < execFunc.flatCold.size()) {
        const auto& fi = execFunc.flatStream[flatIdx];
        const auto& cold = execFunc.flatCold[flatIdx];
        activeSpan = fi.span;
        isTerm =
            fi.flatOpcode >= runtime::detail::FlatOpcode::TermJumpNoArgs ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperLessLocalRegBranch ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperLessLocalConstBranch ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperLessLocalArrayLengthBranch ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump;
        if (cold.inlinedCallee != nullptr && cold.directExecution != nullptr) {
            inlinedLoc = cold.inlinedCallee;
            inlinedExec = cold.directExecution;
            activeBlkIdx = cold.blockIndex;
            activeInstIdx = cold.instructionIndexInBlock;
            callerBlkIdx = cold.callerBlockIndex;
            callerInstIdx = cold.callerInstIndex;
        } else {
            activeBlkIdx = cold.blockIndex;
            activeInstIdx = cold.instructionIndexInBlock;
        }
    }

    const auto makeLoc = [](
        const runtime::ProgramImage::FunctionLocation& loc,
        const runtime::detail::ExecutionFunction& ef,
        std::uint32_t blkIdx,
        std::uint32_t instIdx,
        std::uint32_t span,
        bool term) {
        runtime::StackTraceLocation out;
        out.function = loc.qualifiedName;
        if (!loc.function || !loc.module) return out;
        out.symbolId = loc.function->symbolId;
        const auto& dbg = loc.function->debugInfo;
        if (dbg.sequencePoints.empty()) return out;

        const debug::SequencePoint* bestSp = nullptr;
        if (blkIdx < ef.blocks.size() && ef.blocks[blkIdx].bytecode != nullptr) {
            const auto blkId = ef.blocks[blkIdx].bytecode->id;
            const std::uint32_t maxIdx = instIdx + (span > 1 ? span - 1 : 0);
            for (const auto& sp : dbg.sequencePoints) {
                if (sp.blockId != blkId) continue;
                if (term && sp.terminator) {
                    bestSp = &sp;
                    break;
                }
                if (!sp.terminator && sp.instructionIndex <= maxIdx) {
                    bestSp = &sp;
                }
            }
        }
        if (!bestSp) bestSp = &dbg.sequencePoints.front();
        for (const auto& src : loc.module->sourceFiles) {
            if (src.id == bestSp->range.fileId) {
                out.sourcePath = src.path;
                break;
            }
        }
        out.sourceLine = static_cast<std::uint32_t>(bestSp->range.start.line + 1);
        out.sourceColumn = static_cast<std::uint32_t>(bestSp->range.start.column + 1);
        return out;
    };

    if (inlinedLoc && inlinedExec) {
        auto calleeTrace = makeLoc(*inlinedLoc, *inlinedExec, activeBlkIdx, activeInstIdx, activeSpan, isTerm);
        session.error.stackTrace.push_back(inlinedLoc->qualifiedName);
        session.error.symbolizedStackTrace.push_back(calleeTrace.format());
        session.error.sourceLocations.push_back(std::move(calleeTrace));

        auto callerTrace = makeLoc(location, execFunc, callerBlkIdx, callerInstIdx, 1, false);
        session.error.stackTrace.push_back(location.qualifiedName);
        session.error.symbolizedStackTrace.push_back(callerTrace.format());
        session.error.sourceLocations.push_back(std::move(callerTrace));
    } else {
        auto trace = makeLoc(location, execFunc, activeBlkIdx, activeInstIdx, activeSpan, isTerm);
        session.error.stackTrace.push_back(location.qualifiedName);
        session.error.symbolizedStackTrace.push_back(trace.format());
        session.error.sourceLocations.push_back(std::move(trace));
    }
}

[[nodiscard]] std::uint32_t setHelperError(
    JitAbiContext* ctx,
    std::uint32_t flatIdx,
    runtime::ErrorCode code,
    std::string message) noexcept {
    ctx->errorFlatIndex = flatIdx;
    ctx->errorCodeRaw = static_cast<std::uint32_t>(code);
    if (ctx->session && ctx->compiledFn && ctx->compiledFn->location && ctx->compiledFn->execFunc) {
        populateJitErrorDiagnostics(
            *ctx->session,
            *ctx->compiledFn->location,
            *ctx->compiledFn->execFunc,
            flatIdx,
            code,
            std::move(message));
    }
    return static_cast<std::uint32_t>(code);
}

[[nodiscard]] bool bindJitArrayCacheSlot(
    runtime::ManagedHeap* heap,
    const runtime::Value& arrVal,
    JitArrayCacheEntry& ce) noexcept {
    if (!heap || arrVal.rawTag() != runtime::ValueTag::ObjectRef) return false;
    const runtime::ObjectRef ref = arrVal.u_.objectRef;
    std::uint64_t slotAndGen = 0;
    std::memcpy(&slotAndGen, &arrVal, sizeof(slotAndGen));

    runtime::PackedArrayFastView pView{};
    if (heap->tryBindPackedArrayFastView(ref, pView)) {
        ce.slotAndGen = slotAndGen;
        ce.heapId = ref.heapId;
        ce.kind = static_cast<std::uint8_t>(ref.kind);
        ce.isPacked = 1u;
        ce.encoding = static_cast<std::uint8_t>(pView.encoding);
        ce.elementType = static_cast<std::uint8_t>(pView.elementType);
        ce.rawData = pView.rawData;
        ce.length = pView.length;
        ce.flatStride = 0u;
        ce.flatSchemaId = 0u;
        ce.elementTypeId = static_cast<std::uint64_t>(pView.elementTypeId);
        ce.dirtyCards = nullptr;
        ce.blittableSchemaId = pView.blittableSchemaId;
        ce.flatFlags = 0u;
        return true;
    }

    runtime::FlatStructArrayFastView fView{};
    if (heap->tryBindFlatStructArrayFastView(ref, fView)) {
        ce.slotAndGen = slotAndGen;
        ce.heapId = ref.heapId;
        ce.kind = static_cast<std::uint8_t>(ref.kind);
        ce.isPacked = 2u;
        ce.encoding = 0u;
        ce.elementType = static_cast<std::uint8_t>(semantic::PrimitiveType::Struct);
        ce.rawData = fView.data;
        ce.length = fView.length;
        ce.flatStride = fView.flatStride;
        ce.flatSchemaId = fView.flatSchemaId;
        ce.elementTypeId = static_cast<std::uint64_t>(fView.elementTypeId);
        ce.dirtyCards = fView.dirtyCards;
        ce.blittableSchemaId = 0u;
        ce.flatFlags = static_cast<std::uint8_t>(
            (fView.containsManagedReferences ? 1u : 0u) |
            (fView.containsPoolValues ? 2u : 0u) |
            (fView.needsRememberedBarrier ? 4u : 0u));
        return true;
    }

    runtime::ArrayFastView bView{};
    if (heap->tryBindArrayFastView(ref, bView)) {
        ce.slotAndGen = slotAndGen;
        ce.heapId = ref.heapId;
        ce.kind = static_cast<std::uint8_t>(ref.kind);
        ce.isPacked = 0u;
        ce.encoding = 0u;
        ce.elementType = static_cast<std::uint8_t>(bView.elementType);
        ce.rawData = bView.data;
        ce.length = bView.length;
        ce.flatStride = 0u;
        ce.flatSchemaId = 0u;
        ce.elementTypeId = static_cast<std::uint64_t>(bView.elementTypeId);
        ce.dirtyCards = nullptr;
        ce.blittableSchemaId = 0u;
        ce.flatFlags = 0u;
        return true;
    }
    return false;
}

// Slow-path C++ helper for NewArrayFast
std::uint32_t jitNewArrayHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    const auto& cold = execFunc.flatCold[flatIdx];
    auto& session = *ctx->session;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    std::int64_t lenVal = 0;
    if (!ctx->frameBase[fi.src0Slot].tryGetInt64(lenVal)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array length must be int");
    }
    if (lenVal < 0) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::IndexOutOfRange, "array length cannot be negative");
    }
    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    if (!session.heap) {
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::OutOfMemory, "managed heap is unavailable");
    }

    const auto elemType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1));
    const auto arrayTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
    runtime::Value initVal{};
    semantic::SymbolId elemTid = 0;
    if (fi.aux0Slot == 1) {
        switch (elemType) {
        case semantic::PrimitiveType::Int: initVal.storeInt64(0); break;
        case semantic::PrimitiveType::Float: initVal.storeFloat(0.0f); break;
        case semantic::PrimitiveType::Double: initVal.storeDouble(0.0); break;
        case semantic::PrimitiveType::Long: initVal.storeLong(0); break;
        case semantic::PrimitiveType::Bool: initVal.storeBool(false); break;
        case semantic::PrimitiveType::Byte: initVal = runtime::ByteValue{0}; break;
        case semantic::PrimitiveType::SByte: initVal = runtime::SByteValue{0}; break;
        case semantic::PrimitiveType::Short: initVal = runtime::ShortValue{0}; break;
        case semantic::PrimitiveType::UShort: initVal = runtime::UShortValue{0}; break;
        case semantic::PrimitiveType::UInt: initVal = runtime::UIntValue{0}; break;
        case semantic::PrimitiveType::ULong: initVal = runtime::ULongValue{0}; break;
        case semantic::PrimitiveType::Char: initVal = runtime::CharValue{0}; break;
        case semantic::PrimitiveType::Object:
            initVal = runtime::NullObject{};
            if (cold.bytecode) elemTid = cold.bytecode->elementTypeId;
            break;
        case semantic::PrimitiveType::String:
            initVal = runtime::NullString{};
            break;
        default:
            break;
        }
    } else if (fi.aux0Slot >= 2 && fi.aux0Slot <= 10) {
        if (cold.bytecode) elemTid = cold.bytecode->elementTypeId;
        switch (fi.aux0Slot) {
        case 2: initVal = runtime::FixedValue{}; break;
        case 3: initVal = runtime::Fixed16Value{}; break;
        case 4: initVal = runtime::Vec2Value{}; break;
        case 5: initVal = runtime::Vec3Value{}; break;
        case 6: initVal = runtime::QuatValue{}; break;
        case 7: initVal = runtime::Fixed64Value{}; break;
        case 8: initVal = runtime::Vec2fValue{}; break;
        case 9: initVal = runtime::Vec3fValue{}; break;
        default: initVal = runtime::QuatfValue{}; break;
        }
    } else if (fi.aux0Slot == 11) {
        if (cold.bytecode) elemTid = cold.bytecode->elementTypeId;
        initVal = runtime::Value{};
    } else if (cold.bytecode) {
        elemTid = cold.bytecode->elementTypeId;
        if (elemType == semantic::PrimitiveType::Enum) {
            initVal = runtime::EnumValue{elemTid, 0};
        } else if (elemType == semantic::PrimitiveType::Array) {
            initVal = runtime::NullArray{};
        } else if (elemType == semantic::PrimitiveType::Struct) {
            if (runtime::isBlittableStructType(elemTid) ||
                runtime::findFlatStructSchemaByTypeId(elemTid) != nullptr) {
                initVal = runtime::Value{};
            } else if (auto defStruct = runtime::makeDefaultBlittableStructValue(elemTid)) {
                initVal = std::move(*defStruct);
            }
        }
    }

    bool slotsMoved = false;
    runtime::RuntimeError allocErr;
    const auto ref = session.heap->allocateTypedArray(
        arrayTid,
        elemType,
        elemTid,
        static_cast<std::size_t>(lenVal),
        initVal,
        &allocErr,
        &slotsMoved);
    if (!ref.has_value()) {
        ctx->remainingBudget -= opDoneCost;
        const auto errCode = allocErr.code == runtime::ErrorCode::None
            ? runtime::ErrorCode::OutOfMemory
            : allocErr.code;
        return setHelperError(
            ctx,
            flatIdx,
            errCode,
            allocErr.message.empty() ? "managed array allocation failed" : allocErr.message);
    }

    ctx->remainingBudget -= fi.span;
    if (slotsMoved) {
        invalidateJitArrayCaches(*ctx);
    }
    if (session.limits.gcWorkBudget != 0 &&
        (session.heap->phase() != runtime::GcPhase::Idle || session.heap->collectionRequested())) {
        const auto work = session.heap->step(session.shadowStack, session.limits.gcWorkBudget);
        session.stats.gcWorkPerformed += work;
        invalidateJitArrayCaches(*ctx);
    }
    ctx->frameBase[fi.dstSlot] = *ref;
    (void)bindJitArrayCacheSlot(
        session.heap,
        ctx->frameBase[fi.dstSlot],
        ctx->arrayCache[fi.dstSlot & 3u]);
    return 0u;
}

// Slow-path C++ helper for ArrayLengthFast & SuperLessLocalArrayLengthBranch
std::uint32_t jitArrayLengthHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    auto& session = *ctx->session;
    const bool isBranch = (fi.flatOpcode == runtime::detail::FlatOpcode::SuperLessLocalArrayLengthBranch);
    const std::uint16_t arrSlot = isBranch ? fi.src1Slot : fi.src0Slot;
    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    std::int64_t lhsInt = 0;
    if (isBranch && !ctx->frameBase[fi.src0Slot].tryGetInt64(lhsInt)) {
        if (ctx->remainingBudget < 1u) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= 1u;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "expected int local in array length branch");
    }

    const runtime::Value& arrVal = ctx->frameBase[arrSlot];
    if (std::holds_alternative<runtime::NullArray>(arrVal)) {
        const std::uint64_t nullCost = isBranch ? (fi.span - 2u) : opDoneCost;
        if (ctx->remainingBudget < nullCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= nullCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::NullReference, "array length attempted to dereference null");
    }

    auto& ce = ctx->arrayCache[arrSlot & 3u];
    std::int64_t len64 = -1;
    if (bindJitArrayCacheSlot(session.heap, arrVal, ce)) {
        len64 = static_cast<std::int64_t>(ce.length);
    } else if (const auto* ref = std::get_if<runtime::ObjectRef>(&arrVal); ref && session.heap) {
        if (!session.heap->isAlive(*ref)) {
            if (ctx->remainingBudget < opDoneCost) {
                ctx->remainingBudget = 0;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= opDoneCost;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid array reference");
        }
        if (const auto optLen = session.heap->arrayLength(*ref)) {
            len64 = static_cast<std::int64_t>(*optLen);
        }
    }
    if (len64 < 0) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "expected array operand");
    }

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    ctx->remainingBudget -= fi.span;

    if (!isBranch) {
        ctx->frameBase[fi.dstSlot].storeInt64(len64);
        return 0u;
    }

    bool cond = false;
    switch (fi.binaryOpcode1) {
    case runtime::detail::QuickOpcode::LessInt: cond = (lhsInt < len64); break;
    case runtime::detail::QuickOpcode::LessOrEqualInt: cond = (lhsInt <= len64); break;
    case runtime::detail::QuickOpcode::GreaterInt: cond = (lhsInt > len64); break;
    case runtime::detail::QuickOpcode::GreaterOrEqualInt: cond = (lhsInt >= len64); break;
    case runtime::detail::QuickOpcode::EqualInt: cond = (lhsInt == len64); break;
    case runtime::detail::QuickOpcode::NotEqualInt: cond = (lhsInt != len64); break;
    default:
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "invalid array length comparison");
    }
    ++ctx->branchesTaken;
    ctx->branchCondOut = cond ? 1u : 0u;
    return 0u;
}

// Slow-path C++ helper for LoadElementIntFast & LoadElementTypedFast
std::uint32_t jitLoadElementHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    const auto& cold = execFunc.flatCold[flatIdx];
    auto& session = *ctx->session;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    const runtime::Value& arrVal = ctx->frameBase[fi.src0Slot];
    const runtime::Value& idxVal = ctx->frameBase[fi.src1Slot];

    // Lock-free JitArrayCacheEntry fast path for FlatStruct[] & BlittableStruct[]!
    if (fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementTypedFast &&
        execFunc.hasStructPayloads &&
        session.flatBankId != 0u &&
        arrVal.rawTag() == runtime::ValueTag::ObjectRef &&
        fi.dstSlot < execFunc.slotStructSchemaIds.size()) {
        const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
        const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
        if (sid != 0u && payOff != runtime::detail::InvalidCompactReg) {
            auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
            std::uint64_t sg = 0;
            std::memcpy(&sg, &arrVal, sizeof(sg));
            if (sg != ce.slotAndGen || arrVal.u_.objectRef.heapId != ce.heapId) {
                (void)bindJitArrayCacheSlot(session.heap, arrVal, ce);
            }
            std::int64_t fastIdx = 0;
            if (sg == ce.slotAndGen &&
                arrVal.u_.objectRef.heapId == ce.heapId &&
                idxVal.tryGetInt64(fastIdx) &&
                static_cast<std::uint64_t>(fastIdx) < ce.length) {
                const auto expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
                if (ce.elementTypeId == expectedTid) {
                    if (ctx->remainingBudget < fi.span) {
                        ctx->remainingBudget = 0;
                        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                    }
                    const auto uIdx = static_cast<std::size_t>(fastIdx);
                    if (ce.isPacked == 2u && ce.flatSchemaId == sid) {
                        const runtime::Value* elemSrc =
                            static_cast<const runtime::Value*>(ce.rawData) + uIdx * ce.flatStride;
                        std::memcpy(
                            static_cast<void*>(ctx->frameBase + payOff),
                            static_cast<const void*>(elemSrc),
                            static_cast<std::size_t>(ce.flatStride) * sizeof(runtime::Value));
                        ctx->remainingBudget -= fi.span;
                        ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                            expectedTid,
                            session.flatBankId,
                            static_cast<std::uint32_t>(session.frameOffset + payOff),
                            sid);
                        return 0u;
                    }
                    if (ce.isPacked == 1u &&
                        ce.encoding == static_cast<std::uint8_t>(runtime::PackedElemEncoding::BlittableStruct)) {
                        if (const auto* sc = runtime::getFlatStructSchema(sid)) {
                            if (runtime::decodePackedStructToLeaves(
                                    ce.rawData, uIdx, ce.blittableSchemaId, *sc, ctx->frameBase + payOff)) {
                                ctx->remainingBudget -= fi.span;
                                ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                                    expectedTid,
                                    session.flatBankId,
                                    static_cast<std::uint32_t>(session.frameOffset + payOff),
                                    sid);
                                return 0u;
                            }
                        }
                    }
                }
            }
        }
    }

    if (std::holds_alternative<runtime::NullArray>(arrVal)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::NullReference, "array element load attempted to dereference null");
    }

    if (!session.heap || arrVal.rawTag() != runtime::ValueTag::ObjectRef) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element load expected array");
    }

    const runtime::ObjectRef ref = arrVal.u_.objectRef;
    if (!session.heap->isAlive(ref)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "array element load references an invalid managed array");
    }

    std::int64_t idx = 0;
    if (!idxVal.tryGetInt64(idx)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array index expected int");
    }

    auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
    (void)bindJitArrayCacheSlot(session.heap, arrVal, ce);

    const auto lengthOpt = session.heap->arrayLength(ref);
    if (!lengthOpt) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid managed array");
    }
    if (idx < 0 || static_cast<std::uint64_t>(idx) >= *lengthOpt) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
    }

    semantic::PrimitiveType expectedType = semantic::PrimitiveType::Int;
    semantic::SymbolId expectedTid = 0;
    if (fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementTypedFast) {
        expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1));
        expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
    } else if (cold.bytecode) {
        expectedType = cold.bytecode->elementType;
        expectedTid = cold.bytecode->elementTypeId;
    }

    const auto actualElemType = session.heap->arrayElementType(ref);
    const auto actualElemTid = session.heap->arrayElementTypeId(ref);
    if (!actualElemType || !actualElemTid ||
        *actualElemType != expectedType || *actualElemTid != expectedTid) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element metadata mismatch");
    }

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }

    // Zero-StructStorage fast path when destination slot is a flat-frame struct slice!
    if (expectedType == semantic::PrimitiveType::Struct &&
        execFunc.hasStructPayloads &&
        session.flatBankId != 0u &&
        fi.dstSlot < execFunc.slotStructSchemaIds.size()) {
        const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
        const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
        const auto* sc = sid != 0u ? runtime::getFlatStructSchema(sid) : nullptr;
        if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg) {
            runtime::FlatStructArrayFastView fView{};
            if ((session.heap->tryBindFlatStructArrayFastView(ref, fView) && fView.elementTypeId == expectedTid && fView.flatStride == sc->flatSlotCount) &&
                fView.flatSchemaId == sid) {
                const runtime::Value* elemSrc =
                    fView.data + static_cast<std::size_t>(idx) * fView.flatStride;
                std::memcpy(
                    static_cast<void*>(ctx->frameBase + payOff),
                    static_cast<const void*>(elemSrc),
                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                    expectedTid,
                    session.flatBankId,
                    static_cast<std::uint32_t>(session.frameOffset + payOff),
                    sid);
                return 0u;
            }
            runtime::PackedArrayFastView pView{};
            if (session.heap->tryBindPackedArrayFastView(ref, pView) &&
                pView.encoding == runtime::PackedElemEncoding::BlittableStruct &&
                runtime::decodePackedStructToLeaves(
                    pView.rawData,
                    static_cast<std::size_t>(idx),
                    pView.blittableSchemaId,
                    *sc,
                    ctx->frameBase + payOff)) {
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                    expectedTid,
                    session.flatBankId,
                    static_cast<std::uint32_t>(session.frameOffset + payOff),
                    sid);
                return 0u;
            }
        }
    }

    const auto loaded = session.heap->arrayGet(ref, static_cast<std::size_t>(idx));
    if (!loaded) {
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "array element load failed");
    }

    ctx->remainingBudget -= fi.span;
    if (execFunc.hasStructPayloads &&
        session.flatBankId != 0u &&
        fi.dstSlot < execFunc.slotStructSchemaIds.size() &&
        execFunc.slotStructSchemaIds[fi.dstSlot] != 0u) {
        const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
        const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg &&
            runtime::extractStructLeavesToBuffer(*loaded, *sc, ctx->frameBase + payOff)) {
            ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                sc->structTypeId,
                session.flatBankId,
                static_cast<std::uint32_t>(session.frameOffset + payOff),
                sid);
            return 0u;
        }
    }
    ctx->frameBase[fi.dstSlot] = *loaded;
    return 0u;
}

// Slow-path C++ helper for StoreElementIntFast & StoreElementPrimFast
std::uint32_t jitStoreElementHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    const auto& cold = execFunc.flatCold[flatIdx];
    auto& session = *ctx->session;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    const runtime::Value& arrVal = ctx->frameBase[fi.src0Slot];
    const runtime::Value& idxVal = ctx->frameBase[fi.src1Slot];
    const runtime::Value& storeVal = ctx->frameBase[fi.src2Slot];

    // Lock-free JitArrayCacheEntry fast path for FlatStruct[] & BlittableStruct[] stores!
    if ((fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFieldInPlace ||
         fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFast) &&
        arrVal.rawTag() == runtime::ValueTag::ObjectRef) {
        auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
        std::uint64_t sg = 0;
        std::memcpy(&sg, &arrVal, sizeof(sg));
        if (sg != ce.slotAndGen || arrVal.u_.objectRef.heapId != ce.heapId) {
            (void)bindJitArrayCacheSlot(session.heap, arrVal, ce);
        }
        std::int64_t fastIdx = 0;
        if (sg == ce.slotAndGen &&
            arrVal.u_.objectRef.heapId == ce.heapId &&
            idxVal.tryGetInt64(fastIdx) &&
            static_cast<std::uint64_t>(fastIdx) < ce.length) {
            const auto uIdx = static_cast<std::size_t>(fastIdx);
            const runtime::ObjectRef ref = arrVal.u_.objectRef;
            if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFieldInPlace) {
                const auto expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
                const auto rootSid = static_cast<std::uint16_t>(fi.auxImm32);
                if (ce.elementTypeId == expectedTid) {
                    if (ce.isPacked == 2u && ce.flatSchemaId == rootSid && fi.aux0Slot < ce.flatStride) {
                        if (ctx->remainingBudget < fi.span) {
                            ctx->remainingBudget = 0;
                            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                        }
                        runtime::Value* dstLeaf =
                            static_cast<runtime::Value*>(ce.rawData) + uIdx * ce.flatStride + fi.aux0Slot;
                        bool wroteOk = false;
                        bool needPoolFallback = false;
                        if (fi.aux1Slot == 0u) {
                            const runtime::Value detached = runtime::detachFrameSliceValue(storeVal);
                            if ((ce.flatFlags & 2u) != 0u &&
                                (detached.rawTag() == runtime::ValueTag::String ||
                                 detached.rawTag() == runtime::ValueTag::QuatArena ||
                                 detached.rawTag() == runtime::ValueTag::QuatfArena)) {
                                needPoolFallback = true;
                            } else {
                                *dstLeaf = detached;
                                wroteOk = true;
                            }
                        } else if ((ce.flatFlags & 2u) == 0u) {
                            const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot);
                            if (subSc != nullptr &&
                                fi.aux0Slot + subSc->flatSlotCount <= ce.flatStride &&
                                runtime::extractStructLeavesToBuffer(storeVal, *subSc, dstLeaf)) {
                                wroteOk = true;
                            }
                        }
                        if (wroteOk && !needPoolFallback) {
                            if ((ce.flatFlags & 1u) != 0u) {
                                if (ce.dirtyCards != nullptr) {
                                    ce.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                                }
                                if ((ce.flatFlags & 4u) != 0u ||
                                    (session.heap != nullptr && session.heap->phase() != runtime::GcPhase::Idle)) {
                                    session.heap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                                    ce.flatFlags = static_cast<std::uint8_t>(ce.flatFlags & ~4u);
                                }
                            }
                            ctx->remainingBudget -= fi.span;
                            return 0u;
                        }
                    } else if (ce.isPacked == 1u &&
                               ce.encoding == static_cast<std::uint8_t>(runtime::PackedElemEncoding::BlittableStruct)) {
                        if (const auto* rootSc = runtime::getFlatStructSchema(rootSid)) {
                            if (ctx->remainingBudget < fi.span) {
                                ctx->remainingBudget = 0;
                                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                            }
                            bool needsDemote = false;
                            if (runtime::writePackedStructLeafSlice(
                                    ce.rawData, uIdx, ce.blittableSchemaId, *rootSc,
                                    fi.aux0Slot, fi.aux1Slot, storeVal, needsDemote)) {
                                ctx->remainingBudget -= fi.span;
                                return 0u;
                            }
                        }
                    }
                }
            } else if (ce.isPacked == 2u && (ce.flatFlags & 2u) == 0u) {
                const auto expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
                if (ce.elementTypeId == expectedTid &&
                    runtime::valueType(storeVal) == semantic::PrimitiveType::Struct) {
                    if (const auto* sc = runtime::getFlatStructSchema(ce.flatSchemaId)) {
                        if (ctx->remainingBudget < fi.span) {
                            ctx->remainingBudget = 0;
                            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                        }
                        runtime::Value* dstElem =
                            static_cast<runtime::Value*>(ce.rawData) + uIdx * ce.flatStride;
                        if (runtime::extractStructLeavesToBuffer(storeVal, *sc, dstElem)) {
                            if ((ce.flatFlags & 1u) != 0u) {
                                if (ce.dirtyCards != nullptr) {
                                    ce.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                                }
                                if ((ce.flatFlags & 4u) != 0u ||
                                    (session.heap != nullptr && session.heap->phase() != runtime::GcPhase::Idle)) {
                                    session.heap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                                    ce.flatFlags = static_cast<std::uint8_t>(ce.flatFlags & ~4u);
                                }
                            }
                            ctx->remainingBudget -= fi.span;
                            return 0u;
                        }
                    }
                }
            }
        }
    }

    if (std::holds_alternative<runtime::NullArray>(arrVal)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::NullReference, "array element store attempted to dereference null");
    }

    if (!session.heap || arrVal.rawTag() != runtime::ValueTag::ObjectRef) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element store expected array");
    }

    const runtime::ObjectRef ref = arrVal.u_.objectRef;
    if (!session.heap->isAlive(ref)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "array element store references an invalid managed array");
    }

    std::int64_t idx = 0;
    if (!idxVal.tryGetInt64(idx)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array index expected int");
    }

    const auto lengthOpt = session.heap->arrayLength(ref);
    if (!lengthOpt) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid managed array");
    }
    if (idx < 0 || static_cast<std::uint64_t>(idx) >= *lengthOpt) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
    }

    if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFieldInPlace) {
        if (ctx->remainingBudget < fi.span) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        const auto expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        const auto rootSid = static_cast<std::uint16_t>(fi.auxImm32);
        const auto* rootSc = runtime::getFlatStructSchema(rootSid);
        const auto uIdx = static_cast<std::size_t>(idx);
        runtime::PackedArrayFastView pView{};
        if (rootSc != nullptr &&
            session.heap->tryBindPackedArrayFastView(ref, pView) &&
            pView.encoding == runtime::PackedElemEncoding::BlittableStruct &&
            pView.elementTypeId == expectedTid) {
            bool needsDemote = false;
            if (runtime::writePackedStructLeafSlice(
                    pView.rawData, uIdx, pView.blittableSchemaId, *rootSc,
                    fi.aux0Slot, fi.aux1Slot, storeVal, needsDemote)) {
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
            // Demote packed array via full element reconstruct + arraySet if int32 overflowed
            if (auto elemOpt = session.heap->arrayGet(ref, uIdx); elemOpt.has_value()) {
                std::vector<runtime::Value> tmpLeaves(rootSc->flatSlotCount);
                if (runtime::extractStructLeavesToBuffer(*elemOpt, *rootSc, tmpLeaves.data())) {
                    if (fi.aux1Slot == 0u) {
                        tmpLeaves[fi.aux0Slot] = runtime::detachFrameSliceValue(storeVal);
                    } else if (const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot)) {
                        (void)runtime::extractStructLeavesToBuffer(storeVal, *subSc, tmpLeaves.data() + fi.aux0Slot);
                    }
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(*rootSc, tmpLeaves.data());
                    runtime::RuntimeError demErr;
                    if (session.heap->arraySet(ref, uIdx, rebuilt, &demErr)) {
                        ctx->remainingBudget -= fi.span;
                        invalidateJitArrayCaches(*ctx);
                        (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
                        return 0u;
                    }
                }
            }
        }
        runtime::FlatStructArrayFastView fView{};
        if (rootSc != nullptr &&
            (session.heap->tryBindFlatStructArrayFastView(ref, fView) && fView.elementTypeId == expectedTid && fView.flatStride == rootSc->flatSlotCount) &&
            fView.flatSchemaId == rootSid &&
            fi.aux0Slot < fView.flatStride) {
            runtime::Value* dstLeaf = fView.data + uIdx * fView.flatStride + fi.aux0Slot;
            if (fi.aux1Slot == 0u) {
                *dstLeaf = runtime::detachFrameSliceValue(storeVal);
                if (fView.containsManagedReferences) {
                    if (fView.dirtyCards != nullptr) {
                        fView.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (fView.needsRememberedBarrier || session.heap->phase() != runtime::GcPhase::Idle) {
                        session.heap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                    }
                }
                // If containsPoolValues, delegate via arraySet only when value is pool-backed String/Arena
                if (fView.containsPoolValues &&
                    (dstLeaf->rawTag() == runtime::ValueTag::String ||
                     dstLeaf->rawTag() == runtime::ValueTag::QuatArena ||
                     dstLeaf->rawTag() == runtime::ValueTag::QuatfArena)) {
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(
                        *rootSc, fView.data + uIdx * fView.flatStride);
                    (void)session.heap->arraySet(ref, uIdx, rebuilt, nullptr);
                    invalidateJitArrayCaches(*ctx);
                    (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
                }
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
            const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot);
            if (subSc != nullptr &&
                fi.aux0Slot + subSc->flatSlotCount <= fView.flatStride &&
                runtime::extractStructLeavesToBuffer(storeVal, *subSc, dstLeaf)) {
                if (fView.containsManagedReferences) {
                    if (fView.dirtyCards != nullptr) {
                        fView.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (fView.needsRememberedBarrier || session.heap->phase() != runtime::GcPhase::Idle) {
                        session.heap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                    }
                }
                if (fView.containsPoolValues) {
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(
                        *rootSc, fView.data + uIdx * fView.flatStride);
                    (void)session.heap->arraySet(ref, uIdx, rebuilt, nullptr);
                    invalidateJitArrayCaches(*ctx);
                    (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
                }
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "in-place struct element field store failed");
    }

    semantic::PrimitiveType expectedType = semantic::PrimitiveType::Int;
    if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementPrimFast ||
        fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementObjFast) {
        expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1));
    } else if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFast) {
        expectedType = semantic::PrimitiveType::Struct;
    } else if (cold.bytecode) {
        expectedType = cold.bytecode->elementType;
    }

    const bool nullMatchesExpected =
        (expectedType == semantic::PrimitiveType::Object && std::holds_alternative<runtime::NullObject>(storeVal)) ||
        (expectedType == semantic::PrimitiveType::String && std::holds_alternative<runtime::NullString>(storeVal)) ||
        (expectedType == semantic::PrimitiveType::Array && std::holds_alternative<runtime::NullArray>(storeVal));
    if (!nullMatchesExpected && runtime::valueType(storeVal) != expectedType) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element store value type mismatch");
    }

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }

    // Note: ManagedHeap::arraySet already handles FrameSlice StructValue with ZERO StructStorage allocations!
    runtime::RuntimeError setErr;
    if (!session.heap->arraySet(ref, static_cast<std::size_t>(idx), storeVal, &setErr)) {
        ctx->remainingBudget -= opDoneCost;
        const auto code = setErr.code == runtime::ErrorCode::None
            ? runtime::ErrorCode::TypeMismatch
            : setErr.code;
        return setHelperError(ctx, flatIdx, code, setErr.message.empty() ? "arraySet failed" : setErr.message);
    }

    ctx->remainingBudget -= fi.span;
    invalidateJitArrayCaches(*ctx);
    // Re-bind cache slot in case PackedI32 widened to PackedI64 or GC ran
    (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
    return 0u;
}

bool jitValuesEqual(
    runtime::ManagedHeap* heap,
    const runtime::Value& left,
    const runtime::Value& right,
    bool& outEqual,
    runtime::ErrorCode& outErrCode,
    const char*& outErrMsg) noexcept {
    const auto leftType = runtime::valueType(left);
    if (leftType != runtime::valueType(right)) {
        outEqual = false;
        return true;
    }
    if (leftType == semantic::PrimitiveType::Object ||
        leftType == semantic::PrimitiveType::Array) {
        const auto isValidRef = [&](const runtime::Value& value) noexcept {
            if (value.rawTag() != runtime::ValueTag::ObjectRef) return true;
            const auto& ref = value.asObjectRefUnchecked();
            if (ref.isStackSlotRef()) return true;
            return heap != nullptr && heap->isAlive(ref);
        };
        if (!isValidRef(left) || !isValidRef(right)) {
            outErrCode = runtime::ErrorCode::InvalidObjectReference;
            outErrMsg = "reference equality encountered an invalid managed reference";
            return false;
        }
        outEqual = (left == right);
        return true;
    }
    if (leftType == semantic::PrimitiveType::String) {
        if (std::holds_alternative<runtime::NullString>(left) ||
            std::holds_alternative<runtime::NullString>(right)) {
            outEqual = std::holds_alternative<runtime::NullString>(left) &&
                std::holds_alternative<runtime::NullString>(right);
            return true;
        }
        try {
            const auto extractText = [&](const runtime::Value& value)
                -> std::optional<std::string_view> {
                if (const auto* text = std::get_if<std::string>(&value)) return *text;
                if (value.rawTag() == runtime::ValueTag::String) {
                    return std::string_view(value.asStringUnchecked().str());
                }
                if (const auto* ref = std::get_if<runtime::ObjectRef>(&value)) {
                    return heap ? heap->stringView(*ref) : std::nullopt;
                }
                return std::nullopt;
            };
            const auto a = extractText(left);
            const auto b = extractText(right);
            if (!a || !b) {
                outErrCode = runtime::ErrorCode::InvalidObjectReference;
                outErrMsg = "string equality encountered an invalid managed reference";
                return false;
            }
            outEqual = (*a == *b);
            return true;
        } catch (...) {
            outErrCode = runtime::ErrorCode::InvalidObjectReference;
            outErrMsg = "string equality encountered an invalid managed reference";
            return false;
        }
    }
    if (leftType == semantic::PrimitiveType::Struct) {
        if (left == right) {
            outEqual = true;
            return true;
        }
        if (left.rawTag() == runtime::ValueTag::Struct &&
            right.rawTag() == runtime::ValueTag::Struct) {
            const auto& ls = left.asStructUnchecked();
            const auto& rs = right.asStructUnchecked();
            if (ls.typeId == rs.typeId && ls.fieldCount() == rs.fieldCount()) {
                const auto count = ls.fieldCount();
                for (std::size_t i = 0; i < count; ++i) {
                    const auto lf = runtime::structValueFieldAt(ls, i);
                    const auto rf = runtime::structValueFieldAt(rs, i);
                    if (!lf || !rf) {
                        outEqual = false;
                        return true;
                    }
                    bool fieldEq = false;
                    if (!jitValuesEqual(heap, *lf, *rf, fieldEq, outErrCode, outErrMsg)) {
                        return false;
                    }
                    if (!fieldEq) {
                        outEqual = false;
                        return true;
                    }
                }
                outEqual = true;
                return true;
            }
        }
        outEqual = false;
        return true;
    }
    outEqual = (left == right);
    return true;
}

// Fast C++ runtime helper for FlatStruct slice & StackSlotRef opcodes in StencilJit
std::uint32_t jitFlatStructAndStackRefHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    auto& session = *ctx->session;
    using FO = runtime::detail::FlatOpcode;

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    if (fi.flatOpcode == FO::CheckNotNullFast) {
        const runtime::Value& val = ctx->frameBase[fi.src0Slot];
        if (val.rawTag() == runtime::ValueTag::ObjectRef) {
            const runtime::ObjectRef ref = val.u_.objectRef;
            if ((ref.isStackSlotRef() && ref.generation == session.flatBankId) ||
                (session.heap != nullptr && session.heap->isAlive(ref))) {
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = val;
                return 0u;
            }
        }
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::NullReference, "null object reference");
    }
    if (fi.flatOpcode == FO::NewObjectFast) {
        if (!session.heap || !session.program) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "object allocation requires heap");
        }
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.aux0Slot >= mirror.objectTemplates.size()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "invalid object template");
        }
        const auto& tmpl = mirror.objectTemplates[fi.aux0Slot];
        runtime::RuntimeError allocErr;
        const auto allocated = session.heap->allocateObjectFromTemplate(
            tmpl.typeId,
            tmpl.defaultFields.data(),
            tmpl.defaultFields.size(),
            tmpl.referenceFields,
            &allocErr);
        if (!allocated) {
            return setHelperError(
                ctx,
                flatIdx,
                allocErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::OutOfMemory : allocErr.code,
                allocErr.message.empty() ? "managed object allocation failed" : allocErr.message);
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = *allocated;
        invalidateJitArrayCaches(*ctx);
        return 0u;
    }
    if (fi.flatOpcode == FO::LoadFieldExactFast || fi.flatOpcode == FO::LoadFieldFast) {
        const runtime::Value& recvVal = ctx->frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            const auto errCode = (recvVal.rawTag() == runtime::ValueTag::NullObject)
                ? runtime::ErrorCode::NullReference
                : runtime::ErrorCode::TypeMismatch;
            return setHelperError(ctx, flatIdx, errCode, "field load attempted on null or non-object");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        if (ref.isStackSlotRef()) {
            if (ref.generation != session.flatBankId || !session.values) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid stack ref");
            }
            if (ref.stackRefSchemaId() == 0u) {
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = (*session.values)[ref.slot];
                return 0u;
            }
        }
        if (!session.heap) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "field load missing heap");
        }
        runtime::ObjectFastView view{};
        if (!session.heap->tryBindObjectFastView(ref, view)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "dangling object reference");
        }
        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        bool ownerOk = (expectedOwnerTid == 0) || (view.typeId == expectedOwnerTid);
        if (!ownerOk && session.program) {
            const auto* cur = session.program->resolveType(view.typeId);
            while (cur != nullptr) {
                if (cur->id == expectedOwnerTid) {
                    ownerOk = true;
                    break;
                }
                cur = (cur->baseTypeId != 0) ? session.program->resolveType(cur->baseTypeId) : nullptr;
            }
        }
        if (!ownerOk || fi.aux0Slot >= view.fieldCount) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "field load receiver type mismatch");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = view.fields[fi.aux0Slot];
        return 0u;
    }
    if (fi.flatOpcode == FO::StoreFieldExactFast || fi.flatOpcode == FO::StoreFieldFast) {
        const runtime::Value& recvVal = ctx->frameBase[fi.src0Slot];
        const runtime::Value& storeVal = ctx->frameBase[fi.src1Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            const auto errCode = (recvVal.rawTag() == runtime::ValueTag::NullObject)
                ? runtime::ErrorCode::NullReference
                : runtime::ErrorCode::TypeMismatch;
            return setHelperError(ctx, flatIdx, errCode, "field store attempted on null or non-object");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        if (ref.isStackSlotRef()) {
            if (ref.generation != session.flatBankId || !session.values) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid stack ref");
            }
            if (ref.stackRefSchemaId() == 0u) {
                (*session.values)[ref.slot] = runtime::detachFrameSliceValue(storeVal);
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
        }
        if (!session.heap) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "field store missing heap");
        }
        const auto actualTid = session.heap->objectTypeId(ref);
        if (!actualTid) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "dangling object reference");
        }
        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        bool ownerOk = (expectedOwnerTid == 0) || (*actualTid == expectedOwnerTid);
        if (!ownerOk && session.program) {
            const auto* cur = session.program->resolveType(*actualTid);
            while (cur != nullptr) {
                if (cur->id == expectedOwnerTid) {
                    ownerOk = true;
                    break;
                }
                cur = (cur->baseTypeId != 0) ? session.program->resolveType(cur->baseTypeId) : nullptr;
            }
        }
        if (!ownerOk) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "field store receiver type mismatch");
        }
        runtime::RuntimeError setErr;
        if (!session.heap->fieldSet(ref, fi.aux0Slot, runtime::detachFrameSliceValue(storeVal), &setErr)) {
            return setHelperError(
                ctx,
                flatIdx,
                setErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::InvalidProgram : setErr.code,
                setErr.message.empty() ? "field store failed" : setErr.message);
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    if (fi.flatOpcode == FO::GenericBytecode) {
        if (flatIdx >= execFunc.flatCold.size() ||
            execFunc.flatCold[flatIdx].bytecode == nullptr ||
            execFunc.bytecode == nullptr) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "missing GenericBytecode metadata");
        }
        const auto& bc = *execFunc.flatCold[flatIdx].bytecode;
        const auto& bcFn = *execFunc.bytecode;
        const auto regOff = execFunc.registerOffset;
        switch (bc.opcode) {
        case bytecode::Opcode::ConstantInt: {
            const auto resType = bcFn.registerTypes[bc.result];
            if (resType == semantic::PrimitiveType::Long) {
                ctx->frameBase[regOff + bc.result] = runtime::LongValue{bc.integerImmediate};
            } else if (resType == semantic::PrimitiveType::Enum) {
                const auto tid = bc.result < bcFn.registerTypeIds.size() ? bcFn.registerTypeIds[bc.result] : 0;
                ctx->frameBase[regOff + bc.result] = runtime::EnumValue{tid, bc.integerImmediate};
            } else {
                ctx->frameBase[regOff + bc.result] = bc.integerImmediate;
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        case bytecode::Opcode::ConstantNull: {
            const auto resType = bcFn.registerTypes[bc.result];
            if (resType == semantic::PrimitiveType::Null) {
                ctx->frameBase[regOff + bc.result] = std::monostate{};
            } else if (resType == semantic::PrimitiveType::String) {
                ctx->frameBase[regOff + bc.result] = runtime::NullString{};
            } else if (resType == semantic::PrimitiveType::Array) {
                ctx->frameBase[regOff + bc.result] = runtime::NullArray{};
            } else {
                ctx->frameBase[regOff + bc.result] = runtime::NullObject{};
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        case bytecode::Opcode::ConvertNullToString:
        case bytecode::Opcode::ConvertNullToObject:
        case bytecode::Opcode::ConvertNullToArray: {
            const runtime::Value& src = ctx->frameBase[regOff + bc.operands[0]];
            const auto tag = src.rawTag();
            if (tag != runtime::ValueTag::Monostate &&
                tag != runtime::ValueTag::NullString &&
                tag != runtime::ValueTag::NullObject &&
                tag != runtime::ValueTag::NullArray) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "expected null for conversion");
            }
            if (bc.opcode == bytecode::Opcode::ConvertNullToString) {
                ctx->frameBase[regOff + bc.result] = runtime::NullString{};
            } else if (bc.opcode == bytecode::Opcode::ConvertNullToArray) {
                ctx->frameBase[regOff + bc.result] = runtime::NullArray{};
            } else {
                ctx->frameBase[regOff + bc.result] = runtime::NullObject{};
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        case bytecode::Opcode::ConstantString: {
            if (!session.heap) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "ConstantString requires heap");
            }
            runtime::RuntimeError strErr;
            const auto interned = session.heap->internString(bc.stringImmediate, &strErr);
            if (!interned) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::OutOfMemory, strErr.message);
            }
            ctx->frameBase[regOff + bc.result] = *interned;
            ctx->remainingBudget -= fi.span;
            invalidateJitArrayCaches(*ctx);
            return 0u;
        }
        case bytecode::Opcode::Equal:
        case bytecode::Opcode::NotEqual: {
            const runtime::Value& lhs = ctx->frameBase[regOff + bc.operands[0]];
            const runtime::Value& rhs = ctx->frameBase[regOff + bc.operands[1]];
            bool eq = false;
            runtime::ErrorCode eqErrCode = runtime::ErrorCode::InvalidObjectReference;
            const char* eqErrMsg = "equality comparison failed";
            if (!jitValuesEqual(session.heap, lhs, rhs, eq, eqErrCode, eqErrMsg)) {
                return setHelperError(ctx, flatIdx, eqErrCode, eqErrMsg);
            }
            ctx->frameBase[regOff + bc.result] = (bc.opcode == bytecode::Opcode::Equal) ? eq : !eq;
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        default:
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "unsupported GenericBytecode in StencilJit");
        }
    }
    if (session.flatBankId == 0u || session.values == nullptr) {
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "flat stack bank is inactive in StencilJit");
    }

    runtime::Value* frameBase = ctx->frameBase;
    const std::size_t frameOffset = session.frameOffset;

    switch (fi.flatOpcode) {
    case FO::InitStructSlice: {
        const std::uint16_t sid = fi.aux0Slot;
        const auto* sc = runtime::getFlatStructSchema(sid);
        const std::uint32_t payOff = fi.auxImm32;
        if (!sc || payOff + sc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + payOff > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "invalid InitStructSlice metadata");
        }
        std::memcpy(
            static_cast<void*>(frameBase + payOff),
            static_cast<const void*>(sc->prototypeLeaves.data()),
            static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            static_cast<semantic::SymbolId>(fi.integerImmediate),
            session.flatBankId,
            static_cast<std::uint32_t>(frameOffset + payOff),
            sid);
        return 0u;
    }
    case FO::CopyStructSlice: {
        const std::uint16_t sid = fi.aux0Slot;
        const std::uint32_t dstPayOff = fi.auxImm32;
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (!sc || dstPayOff + sc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + dstPayOff > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "invalid CopyStructSlice metadata");
        }
        const runtime::Value& srcVal = frameBase[fi.src0Slot];
        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + dstPayOff);
        if (srcVal.rawTag() == runtime::ValueTag::Struct &&
            srcVal.asStructUnchecked().isFrameSlice() &&
            srcVal.asStructUnchecked().frameSchemaId() == sid &&
            srcVal.asStructUnchecked().storage.frameSliceBank() == session.flatBankId) {
            const std::uint32_t srcAbs = srcVal.asStructUnchecked().storage.frameSliceOffset();
            if (srcAbs != dstAbs) {
                std::memmove(
                    static_cast<void*>(session.values->data() + dstAbs),
                    static_cast<const void*>(session.values->data() + srcAbs),
                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
            }
        } else if (!runtime::extractStructLeavesToBuffer(srcVal, *sc, frameBase + dstPayOff)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "CopyStructSlice source mismatch");
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            static_cast<semantic::SymbolId>(fi.integerImmediate),
            session.flatBankId,
            dstAbs,
            sid);
        return 0u;
    }
    case FO::LoadStructSliceField: {
        const runtime::Value& recv = frameBase[fi.src0Slot];
        if (recv.rawTag() != runtime::ValueTag::Struct) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStructSliceField invalid slice");
        }
        const std::uint16_t subSid = fi.aux1Slot;
        const auto subTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        if (!recv.asStructUnchecked().isFrameSlice()) {
            const auto* recvSc = runtime::findFlatStructSchemaByTypeId(recv.asStructUnchecked().typeId);
            if (!recvSc || fi.aux0Slot >= recvSc->flatSlotCount) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStructSliceField invalid detached struct");
            }
            std::vector<runtime::Value> tmpLeaves(recvSc->flatSlotCount);
            if (!runtime::extractStructLeavesToBuffer(recv, *recvSc, tmpLeaves.data())) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStructSliceField detached extract failed");
            }
            if (subSid == 0u) {
                ctx->remainingBudget -= fi.span;
                frameBase[fi.dstSlot] = tmpLeaves[fi.aux0Slot];
                return 0u;
            }
            const auto* subSc = runtime::getFlatStructSchema(subSid);
            if (!subSc || static_cast<std::uint32_t>(fi.aux0Slot) + subSc->flatSlotCount > recvSc->flatSlotCount) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField invalid detached sub-slice");
            }
            if (fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
                ctx->remainingBudget -= fi.span;
                frameBase[fi.dstSlot] = runtime::reconstructDetachedStructFromLeaves(*subSc, tmpLeaves.data() + fi.aux0Slot);
                return 0u;
            }
            if (fi.auxImm32 + subSc->flatSlotCount > execFunc.totalFrameSlots ||
                frameOffset + fi.auxImm32 > runtime::StructStorageHandle::FrameOffsetMask) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField invalid sub-slice");
            }
            const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + fi.auxImm32);
            for (std::uint16_t k = 0; k < subSc->flatSlotCount; ++k) {
                (*session.values)[dstAbs + k] = tmpLeaves[fi.aux0Slot + k];
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                subTid, session.flatBankId, dstAbs, subSid);
            return 0u;
        }
        if (recv.asStructUnchecked().storage.frameSliceBank() != session.flatBankId) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStructSliceField invalid slice");
        }
        const std::uint64_t baseAbs64 =
            static_cast<std::uint64_t>(recv.asStructUnchecked().storage.frameSliceOffset()) + fi.aux0Slot;
        if (subSid == 0u) {
            if (baseAbs64 >= session.values->size()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField out of bounds");
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = (*session.values)[static_cast<std::size_t>(baseAbs64)];
            return 0u;
        }
        if (fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
            if (baseAbs64 > runtime::StructStorageHandle::FrameOffsetMask) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField offset overflow");
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                subTid, session.flatBankId, static_cast<std::uint32_t>(baseAbs64), subSid);
            return 0u;
        }
        const auto* subSc = runtime::getFlatStructSchema(subSid);
        if (!subSc || fi.auxImm32 + subSc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + fi.auxImm32 > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField invalid sub-slice");
        }
        const std::uint32_t baseAbs = static_cast<std::uint32_t>(baseAbs64);
        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + fi.auxImm32);
        if (dstAbs != baseAbs) {
            std::memmove(
                static_cast<void*>(session.values->data() + dstAbs),
                static_cast<const void*>(session.values->data() + baseAbs),
                static_cast<std::size_t>(subSc->flatSlotCount) * sizeof(runtime::Value));
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            subTid, session.flatBankId, dstAbs, subSid);
        return 0u;
    }
    case FO::StoreStructSliceField: {
        const std::uint16_t sid = fi.aux0Slot;
        const std::uint16_t fIdx = fi.aux1Slot;
        const std::uint32_t dstPayOff = fi.auxImm32;
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (!sc || fIdx >= sc->directFields.size() || dstPayOff + sc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + dstPayOff > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "StoreStructSliceField invalid metadata");
        }
        const runtime::Value& recv = frameBase[fi.src0Slot];
        const runtime::Value& rhs = frameBase[fi.src1Slot];
        runtime::Value* dstLeaves = frameBase + dstPayOff;
        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + dstPayOff);
        if (recv.rawTag() == runtime::ValueTag::Struct &&
            recv.asStructUnchecked().isFrameSlice() &&
            recv.asStructUnchecked().frameSchemaId() == sid &&
            recv.asStructUnchecked().storage.frameSliceBank() == session.flatBankId) {
            const std::uint32_t srcAbs = recv.asStructUnchecked().storage.frameSliceOffset();
            if (srcAbs != dstAbs) {
                std::memmove(
                    static_cast<void*>(dstLeaves),
                    static_cast<const void*>(session.values->data() + srcAbs),
                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
            }
        } else if (!runtime::extractStructLeavesToBuffer(recv, *sc, dstLeaves)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreStructSliceField receiver mismatch");
        }
        const auto& fd = sc->directFields[fIdx];
        if (fd.subSchemaId == 0u) {
            dstLeaves[fd.leafOffset] = rhs;
        } else {
            const auto* subSc = runtime::getFlatStructSchema(fd.subSchemaId);
            if (!subSc || !runtime::extractStructLeavesToBuffer(rhs, *subSc, dstLeaves + fd.leafOffset)) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreStructSliceField field mismatch");
            }
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            static_cast<semantic::SymbolId>(fi.integerImmediate),
            session.flatBankId,
            dstAbs,
            sid);
        return 0u;
    }
    case FO::StoreLocalStructFieldInPlace: {
        const std::uint32_t locPayOff = fi.auxImm32;
        const std::uint16_t leafOff = fi.aux0Slot;
        const std::uint16_t subSid = fi.aux1Slot;
        const runtime::Value& rhs = frameBase[fi.src1Slot];
        if (subSid == 0u) {
            frameBase[locPayOff + leafOff] = rhs;
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        const auto* subSc = runtime::getFlatStructSchema(subSid);
        if (!subSc || !runtime::extractStructLeavesToBuffer(rhs, *subSc, frameBase + locPayOff + leafOff)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreLocalStructFieldInPlace sub-struct mismatch");
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    case FO::InitStackRefCell: {
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.aux0Slot >= mirror.stackRefTemplates.size() || fi.auxImm32 >= execFunc.totalFrameSlots) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "InitStackRefCell invalid template");
        }
        const auto& srt = mirror.stackRefTemplates[fi.aux0Slot];
        const std::uint32_t cellOff = fi.auxImm32;
        if (srt.flatSchemaId == 0u) {
            frameBase[cellOff] = srt.scalarDefaultValue;
        } else {
            const auto* sc = runtime::getFlatStructSchema(srt.flatSchemaId);
            if (!sc || cellOff + sc->flatSlotCount > execFunc.totalFrameSlots) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "InitStackRefCell invalid struct slice");
            }
            std::memcpy(
                static_cast<void*>(frameBase + cellOff),
                static_cast<const void*>(sc->prototypeLeaves.data()),
                static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::ObjectRef::makeStackSlotRef(
            static_cast<std::uint32_t>(frameOffset + cellOff),
            session.flatBankId,
            srt.flatSchemaId);
        return 0u;
    }
    case FO::BindLocalStackRef: {
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::ObjectRef::makeStackSlotRef(
            static_cast<std::uint32_t>(frameOffset + fi.auxImm32),
            session.flatBankId,
            fi.aux0Slot);
        return 0u;
    }
    case FO::WritebackLocalStackRef: {
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    case FO::LoadStackRefCell: {
        const runtime::Value& recvVal = frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "LoadStackRefCell invalid ref");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        const std::uint16_t sid = fi.aux0Slot;
        if (!ref.isStackSlotRef()) {
            const auto fv = session.heap ? session.heap->fieldGet(ref, 0) : std::nullopt;
            if (!fv) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "LoadStackRefCell dangling heap ref");
            }
            if (sid == 0u || fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
                ctx->remainingBudget -= fi.span;
                frameBase[fi.dstSlot] = *fv;
                return 0u;
            }
            const auto* sc = runtime::getFlatStructSchema(sid);
            if (!sc || fi.auxImm32 + sc->flatSlotCount > execFunc.totalFrameSlots ||
                frameOffset + fi.auxImm32 > runtime::StructStorageHandle::FrameOffsetMask) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStackRefCell invalid payload");
            }
            const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + fi.auxImm32);
            if (!runtime::extractStructLeavesToBuffer(*fv, *sc, session.values->data() + dstAbs)) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStackRefCell heap struct mismatch");
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                static_cast<semantic::SymbolId>(fi.integerImmediate), session.flatBankId, dstAbs, sid);
            return 0u;
        }
        if (ref.generation != session.flatBankId) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "LoadStackRefCell invalid ref");
        }
        if (sid == 0u) {
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = (*session.values)[ref.slot];
            return 0u;
        }
        const auto innerTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        if (fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
            if (ref.slot > runtime::StructStorageHandle::FrameOffsetMask) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStackRefCell slice offset overflow");
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                innerTid, session.flatBankId, ref.slot, sid);
            return 0u;
        }
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (!sc || fi.auxImm32 + sc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + fi.auxImm32 > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStackRefCell invalid payload");
        }
        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + fi.auxImm32);
        if (dstAbs != ref.slot) {
            std::memmove(
                static_cast<void*>(session.values->data() + dstAbs),
                static_cast<const void*>(session.values->data() + ref.slot),
                static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            innerTid, session.flatBankId, dstAbs, sid);
        return 0u;
    }
    case FO::StoreStackRefCell: {
        const runtime::Value& recvVal = frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "StoreStackRefCell invalid ref");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        const runtime::Value& rhs = frameBase[fi.src1Slot];
        if (!ref.isStackSlotRef()) {
            if (!session.heap) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "StoreStackRefCell missing heap");
            }
            runtime::RuntimeError setErr;
            if (!session.heap->fieldSet(ref, 0, runtime::detachFrameSliceValue(rhs), &setErr)) {
                return setHelperError(
                    ctx,
                    flatIdx,
                    setErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::InvalidProgram : setErr.code,
                    setErr.message.empty() ? "StoreStackRefCell heap write failed" : setErr.message);
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        if (ref.generation != session.flatBankId) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "StoreStackRefCell invalid ref");
        }
        const std::uint16_t sid = fi.aux0Slot;
        if (sid == 0u) {
            (*session.values)[ref.slot] = runtime::detachFrameSliceValue(rhs);
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (!sc || !runtime::extractStructLeavesToBuffer(rhs, *sc, session.values->data() + ref.slot)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreStackRefCell struct mismatch");
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    case FO::StoreStackRefStructFieldInPlace: {
        const runtime::Value& recvVal = frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef ||
            !recvVal.u_.objectRef.isStackSlotRef() ||
            recvVal.u_.objectRef.generation != session.flatBankId) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "StoreStackRefStructFieldInPlace invalid ref");
        }
        const std::uint32_t dstAbs = recvVal.u_.objectRef.slot + fi.aux0Slot;
        const std::uint16_t subSid = fi.aux1Slot;
        const runtime::Value& rhs = frameBase[fi.src1Slot];
        if (subSid == 0u) {
            (*session.values)[dstAbs] = runtime::detachFrameSliceValue(rhs);
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        const auto* subSc = runtime::getFlatStructSchema(subSid);
        if (!subSc || !runtime::extractStructLeavesToBuffer(rhs, *subSc, session.values->data() + dstAbs)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreStackRefStructFieldInPlace sub-struct mismatch");
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    default:
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "unsupported flat struct/ref op in StencilJit");
    }
}

// Helper for InlineCallEnter (tracks recursion limit, functionCalls, maximumCallDepth, and frame setup)
std::uint32_t jitInlineCallEnterHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    auto& session = *ctx->session;

    if (ctx->remainingBudget < 1u) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    ctx->remainingBudget -= 1u;
    if (session.callDepth >= session.limits.recursionLimit) {
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::RecursionLimitExceeded, "recursion limit exceeded");
    }
    ++session.stats.functionCalls;
    if (session.callDepth + 1 > session.stats.maximumCallDepth) {
        session.stats.maximumCallDepth = session.callDepth + 1;
    }

    runtime::Value* const calleeBase = ctx->frameBase + fi.dstSlot;
    const std::uint32_t nArgs = fi.inlineArgCount;
    const std::uint32_t nLocals = fi.aux0Slot;
    if ((fi.arithFlags & runtime::detail::FlatInstruction::FlagFastParamPrologue) != 0u &&
        ctx->remainingBudget >= 2u * nArgs) {
        runtime::Value* const calleeLocals = calleeBase + nArgs;
        if (nArgs >= 1u) calleeLocals[0] = ctx->frameBase[fi.src0Slot];
        if (nArgs >= 2u) calleeLocals[1] = ctx->frameBase[fi.src1Slot];
        if (nArgs >= 3u) calleeLocals[2] = ctx->frameBase[fi.src2Slot];
        if (nArgs >= 4u) calleeLocals[3] = ctx->frameBase[fi.aux1Slot];
        if (nLocals > nArgs) {
            std::memset(static_cast<void*>(calleeLocals + nArgs), 0, (nLocals - nArgs) * sizeof(runtime::Value));
        }
        ctx->remainingBudget -= 2u * nArgs;
        ctx->nextFlatOffsetOut = 1u; // jump to flatIdx + 1 + nArgs
        return 0u;
    }
    if (nArgs >= 1u) calleeBase[0] = ctx->frameBase[fi.src0Slot];
    if (nArgs >= 2u) calleeBase[1] = ctx->frameBase[fi.src1Slot];
    if (nArgs >= 3u) calleeBase[2] = ctx->frameBase[fi.src2Slot];
    if (nArgs >= 4u) calleeBase[3] = ctx->frameBase[fi.aux1Slot];
    if (nLocals != 0u) {
        std::memset(static_cast<void*>(calleeBase + nArgs), 0, nLocals * sizeof(runtime::Value));
    }
    ctx->nextFlatOffsetOut = 0u;
    return 0u;
}

// Helper for mid-superinstruction budget exhaustion (exact partial sub-instruction accounting)
std::uint32_t jitPartialSuperBudgetHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];

    if ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) {
        if (ctx->remainingBudget == 0u) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= 1u;
        ++ctx->branchesTaken;
        flatIdx = fi.auxImm32;
    }

    if (fi.flatOpcode == runtime::detail::FlatOpcode::TermJumpNoArgs && fi.span == 2u) {
        if (ctx->remainingBudget >= 1u) {
            ctx->remainingBudget = 0;
            ++ctx->branchesTaken;
            flatIdx = fi.auxImm32;
        }
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }

    // Check if a binary arithmetic sub-instruction traps with overflow/div0 BEFORE running out of budget
    const auto& curFi = execFunc.flatStream[flatIdx];
    const auto checkIntTrapAtSubStep = [&](
        runtime::detail::QuickOpcode qop,
        bool checked,
        std::int64_t lhs,
        std::int64_t rhs,
        std::uint64_t stepNeeded) -> std::uint32_t {
        if (ctx->remainingBudget < stepNeeded) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        if (qop == runtime::detail::QuickOpcode::DivideInt) {
            if (rhs == 0) {
                ctx->remainingBudget -= stepNeeded;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::DivisionByZero, "division by zero");
            }
            if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) {
                ctx->remainingBudget -= stepNeeded;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::IntegerOverflow, "integer division overflow");
            }
        } else if (qop == runtime::detail::QuickOpcode::RemainderInt) {
            if (rhs == 0) {
                ctx->remainingBudget -= stepNeeded;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::DivisionByZero, "remainder by zero");
            }
        } else if (checked) {
            std::int64_t r = 0;
            bool ovf = false;
            if (qop == runtime::detail::QuickOpcode::AddInt) {
                r = lhs + rhs;
                ovf = !fitsInt32(r);
            } else if (qop == runtime::detail::QuickOpcode::SubtractInt) {
                r = lhs - rhs;
                ovf = !fitsInt32(r);
            } else if (qop == runtime::detail::QuickOpcode::MultiplyInt) {
                r = lhs * rhs;
                ovf = !fitsInt32(r);
            }
            if (ovf) {
                ctx->remainingBudget -= stepNeeded;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::IntegerOverflow, "integer overflow");
            }
        }
        return 0u;
    };

    switch (curFi.flatOpcode) {
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntConstStore:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntConstStoreElided:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump: {
        std::int64_t lhs = 0;
        if (ctx->frameBase[curFi.src0Slot].tryGetInt64(lhs)) {
            if (const auto trap = checkIntTrapAtSubStep(curFi.binaryOpcode1, curFi.checked1(), lhs, curFi.integerImmediate, 3u)) {
                return trap;
            }
        }
        break;
    }
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntLocalStore:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntLocalStoreElided:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump: {
        std::int64_t lhs = 0, rhs = 0;
        if (ctx->frameBase[curFi.src0Slot].tryGetInt64(lhs) &&
            ctx->frameBase[curFi.src1Slot].tryGetInt64(rhs)) {
            if (const auto trap = checkIntTrapAtSubStep(curFi.binaryOpcode1, curFi.checked1(), lhs, rhs, 3u)) {
                return trap;
            }
        }
        break;
    }
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntRegStore:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntRegStoreElided:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump: {
        std::int64_t lhs = 0, rhs = 0;
        if (ctx->frameBase[curFi.src0Slot].tryGetInt64(lhs) &&
            ctx->frameBase[curFi.src1Slot].tryGetInt64(rhs)) {
            if (const auto trap = checkIntTrapAtSubStep(curFi.binaryOpcode1, curFi.checked1(), lhs, rhs, 2u)) {
                return trap;
            }
        }
        break;
    }
    default:
        break;
    }

    ctx->remainingBudget = 0;
    return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
}

[[nodiscard]] std::uint8_t computeUnexecutedAfterOp(
    const runtime::detail::ExecutionFunction& execFunc,
    std::size_t flatIdx) noexcept {
    const auto& fi = execFunc.flatStream[flatIdx];
    if (fi.span <= 1u || flatIdx >= execFunc.flatCold.size()) return 0u;
    const auto& cold = execFunc.flatCold[flatIdx];
    using FO = runtime::detail::FlatOpcode;
    switch (fi.flatOpcode) {
    case FO::SuperBinaryLocalIntConst:
    case FO::SuperBinaryLocalIntLocal:
    case FO::SuperBinaryLocalIntReg:
        return 0u;
    case FO::SuperBinaryLocalIntConstStore:
    case FO::SuperBinaryLocalIntConstStoreElided:
    case FO::SuperBinaryLocalIntLocalStore:
    case FO::SuperBinaryLocalIntLocalStoreElided:
    case FO::SuperBinaryLocalIntRegStore:
    case FO::SuperBinaryLocalIntRegStoreElided:
        return 1u;
    case FO::SuperBinaryLocalIntConstStoreJump:
    case FO::SuperBinaryLocalIntLocalStoreJump:
    case FO::SuperBinaryLocalIntRegStoreJump:
        return 2u;
    default:
        break;
    }
    const auto* activeExec = (cold.inlinedCallee != nullptr && cold.directExecution != nullptr)
        ? cold.directExecution
        : &execFunc;
    if (cold.bytecode != nullptr && cold.blockIndex < activeExec->blocks.size()) {
        const auto& blk = activeExec->blocks[cold.blockIndex];
        const std::size_t start = cold.instructionIndexInBlock;
        const std::size_t limit = std::min<std::size_t>(blk.instructions.size(), start + fi.span);
        for (std::size_t m = start; m < limit; ++m) {
            if (blk.instructions[m].bytecode == cold.bytecode) {
                const std::size_t done = (m - start) + 1u;
                if (done <= fi.span) {
                    return static_cast<std::uint8_t>(fi.span - done);
                }
            }
        }
    }
    return 0u;
}

[[nodiscard]] bool isStencilSupportedFlatOp(
    const runtime::detail::ExecutionFunction& execFunc,
    const runtime::detail::FlatInstruction& fi) noexcept {
    using FO = runtime::detail::FlatOpcode;
    const auto validSlot = [&](std::uint16_t s) noexcept {
        return s < execFunc.totalFrameSlots;
    };
    const auto validQuickIntOp = [](runtime::detail::QuickOpcode op) noexcept {
        using QO = runtime::detail::QuickOpcode;
        return (op >= QO::AddInt && op <= QO::NotEqualInt);
    };
    const auto validIntCmpOp = [](runtime::detail::QuickOpcode op) noexcept {
        using QO = runtime::detail::QuickOpcode;
        return (op >= QO::LessInt && op <= QO::NotEqualInt);
    };
    const auto validDoubleArithOp = [](runtime::detail::QuickOpcode op) noexcept {
        using QO = runtime::detail::QuickOpcode;
        return (op >= QO::AddDouble && op <= QO::DivideDouble);
    };
    const auto validFloatArithOp = [](runtime::detail::QuickOpcode op) noexcept {
        using QO = runtime::detail::QuickOpcode;
        return (op >= QO::AddFloat && op <= QO::DivideFloat);
    };

    switch (fi.flatOpcode) {
    case FO::MoveSlot:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::SuperMoveParamStoreLocal:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) && validSlot(fi.storeSlot);
    case FO::SuperMoveParamStoreLocalElided:
        return validSlot(fi.src0Slot) && validSlot(fi.storeSlot);
    case FO::ConstantInt:
        return validSlot(fi.dstSlot) && fitsInt32(fi.integerImmediate);
    case FO::ConstantDouble:
    case FO::ConstantBool:
    case FO::ConstantLong:
    case FO::SuperConstFloat:
        return validSlot(fi.dstSlot);
    case FO::SuperConstFloatStoreLocal:
        return validSlot(fi.storeSlot);
    case FO::AddIntChecked:
    case FO::AddIntUnchecked:
    case FO::SubIntChecked:
    case FO::SubIntUnchecked:
    case FO::MulIntChecked:
    case FO::MulIntUnchecked:
    case FO::DivInt:
    case FO::RemInt:
    case FO::LessInt:
    case FO::LessOrEqualInt:
    case FO::GreaterInt:
    case FO::GreaterOrEqualInt:
    case FO::EqualInt:
    case FO::NotEqualInt:
    case FO::AddIntTrustedChecked:
    case FO::AddIntTrustedUnchecked:
    case FO::SubIntTrustedChecked:
    case FO::SubIntTrustedUnchecked:
    case FO::MulIntTrustedChecked:
    case FO::MulIntTrustedUnchecked:
    case FO::LessIntTrusted:
    case FO::AddDouble:
    case FO::SubDouble:
    case FO::MulDouble:
    case FO::DivDouble:
    case FO::LessDouble:
    case FO::LessOrEqualDouble:
    case FO::GreaterDouble:
    case FO::GreaterOrEqualDouble:
    case FO::AddDoubleTrusted:
    case FO::SubDoubleTrusted:
    case FO::MulDoubleTrusted:
    case FO::DivDoubleTrusted:
    case FO::AddFloat:
    case FO::SubFloat:
    case FO::MulFloat:
    case FO::DivFloat:
    case FO::LessFloat:
    case FO::LessOrEqualFloat:
    case FO::GreaterFloat:
    case FO::GreaterOrEqualFloat:
    case FO::EqualFloat:
    case FO::NotEqualFloat:
    case FO::AddFloatTrusted:
    case FO::SubFloatTrusted:
    case FO::MulFloatTrusted:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::NegateIntUnchecked:
    case FO::NegateDouble:
    case FO::NegateFloat:
    case FO::LogicalNot:
    case FO::ConvertIntToLongFast:
    case FO::ConvertIntToDoubleFast:
    case FO::ConvertLongToDoubleFast:
    case FO::ConvertIntToFloatFast:
    case FO::ConvertFloatToDoubleFast:
    case FO::ConvertDoubleToFloatFast:
    case FO::ConvertFloatToIntFast:
    case FO::ConvertLongToIntFast:
    case FO::ArrayLengthFast:
    case FO::CheckNotNullFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::NewArrayFast: {
        if (!validSlot(fi.dstSlot) || !validSlot(fi.src0Slot)) return false;
        const auto elemType = static_cast<semantic::PrimitiveType>(
            static_cast<std::uint8_t>(fi.binaryOpcode1));
        if (fi.aux0Slot == 0 && elemType == semantic::PrimitiveType::Struct) {
            const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
            if (idx >= execFunc.flatCold.size() || execFunc.flatCold[idx].bytecode == nullptr) {
                return false;
            }
            const auto elemTid = execFunc.flatCold[idx].bytecode->elementTypeId;
            return runtime::isBlittableStructType(elemTid) ||
                   runtime::findFlatStructSchemaByTypeId(elemTid) != nullptr;
        }
        return true;
    }
    case FO::SuperBinaryLocalIntConst:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) &&
               validSlot(fi.aux0Slot) && validSlot(fi.aux1Slot) &&
               fitsInt32(fi.integerImmediate) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntConstStore:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) &&
               validSlot(fi.aux0Slot) && validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               fitsInt32(fi.integerImmediate) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntConstStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.storeSlot) &&
               fitsInt32(fi.integerImmediate) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntLocal:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) && validSlot(fi.aux1Slot) &&
               validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntLocalStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) &&
               validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntLocalStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntReg:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux1Slot) &&
               validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntRegStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntRegStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntRegChainStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.src2Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) &&
               validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validQuickIntOp(fi.binaryOpcode1) && validQuickIntOp(fi.binaryOpcode2);
    case FO::SuperBinaryLocalIntRegChainStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.src2Slot) &&
               validSlot(fi.storeSlot) &&
               validQuickIntOp(fi.binaryOpcode1) && validQuickIntOp(fi.binaryOpcode2);
    case FO::SuperBinaryLocalIntConstStoreJump:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) &&
               fitsInt32(fi.integerImmediate) && validQuickIntOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::SuperBinaryLocalIntLocalStoreJump:
    case FO::SuperBinaryLocalIntRegStoreJump:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.dstSlot) &&
               validQuickIntOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::SuperLessLocalConstBranch:
        return validSlot(fi.src0Slot) && fitsInt32(fi.integerImmediate) &&
               validIntCmpOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::SuperLessLocalRegBranch:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validIntCmpOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::SuperLessLocalArrayLengthBranch:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validIntCmpOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::SuperBinaryLocalDoubleConstStore:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) &&
               validSlot(fi.aux0Slot) && validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validDoubleArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalDoubleConstStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.storeSlot) &&
               validDoubleArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalDoubleLocalStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) &&
               validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validDoubleArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalDoubleLocalStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validDoubleArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatLocalStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) &&
               validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatLocalStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatReg:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux1Slot) &&
               validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatRegElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.aux1Slot) && validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatRegStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatRegStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryRegFloatStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validFloatArithOp(fi.binaryOpcode1);
    case FO::LoadElementIntFast:
    case FO::LoadElementTypedFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::StoreElementIntFast:
    case FO::StoreElementPrimFast:
    case FO::StoreElementStructFast:
    case FO::StoreElementObjFast:
    case FO::StoreElementStructFieldInPlace:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.src2Slot);
    case FO::InitStructSlice:
    case FO::InitStackRefCell:
    case FO::BindLocalStackRef:
        return validSlot(fi.dstSlot) && fi.auxImm32 < execFunc.totalFrameSlots;
    case FO::AliasStructSlice:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::CopyStructSlice:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && fi.auxImm32 < execFunc.totalFrameSlots;
    case FO::LoadStructSliceField:
    case FO::LoadStackRefCell:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) &&
               (fi.auxImm32 == runtime::detail::InvalidStreamOffset || fi.auxImm32 < execFunc.totalFrameSlots);
    case FO::StoreStructSliceField:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               fi.auxImm32 < execFunc.totalFrameSlots;
    case FO::StoreLocalStructFieldInPlace:
        return validSlot(fi.src1Slot) && (fi.auxImm32 + fi.aux0Slot) < execFunc.totalFrameSlots;
    case FO::WritebackLocalStackRef:
        return validSlot(fi.src0Slot) && fi.auxImm32 < execFunc.totalFrameSlots;
    case FO::StoreStackRefCell:
    case FO::StoreStackRefStructFieldInPlace:
    case FO::StoreFieldExactFast:
    case FO::StoreFieldFast:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::LoadFieldExactFast:
    case FO::LoadFieldFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::NewObjectFast:
        return validSlot(fi.dstSlot);
    case FO::GenericBytecode: {
        const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
        if (idx >= execFunc.flatCold.size() ||
            execFunc.flatCold[idx].inlinedCallee != nullptr ||
            execFunc.flatCold[idx].bytecode == nullptr ||
            execFunc.bytecode == nullptr) {
            return false;
        }
        const auto& bc = *execFunc.flatCold[idx].bytecode;
        const auto& bcFn = *execFunc.bytecode;
        const auto regCnt = bcFn.registerTypes.size();
        const auto regOff = execFunc.registerOffset;
        switch (bc.opcode) {
        case bytecode::Opcode::ConstantInt: {
            if (bc.result >= regCnt || !validSlot(static_cast<std::uint16_t>(regOff + bc.result))) return false;
            const auto rt = bcFn.registerTypes[bc.result];
            return rt == semantic::PrimitiveType::Int ||
                   rt == semantic::PrimitiveType::Long ||
                   rt == semantic::PrimitiveType::Enum;
        }
        case bytecode::Opcode::ConstantNull:
        case bytecode::Opcode::ConstantString:
            return bc.result < regCnt && validSlot(static_cast<std::uint16_t>(regOff + bc.result));
        case bytecode::Opcode::ConvertNullToString:
        case bytecode::Opcode::ConvertNullToObject:
        case bytecode::Opcode::ConvertNullToArray:
            return bc.result < regCnt &&
                   bc.operands.size() == 1 &&
                   bc.operands[0] < regCnt &&
                   validSlot(static_cast<std::uint16_t>(regOff + bc.result)) &&
                   validSlot(static_cast<std::uint16_t>(regOff + bc.operands[0]));
        case bytecode::Opcode::Equal:
        case bytecode::Opcode::NotEqual: {
            if (bc.result >= regCnt || bc.operands.size() != 2 ||
                bc.operands[0] >= regCnt || bc.operands[1] >= regCnt) {
                return false;
            }
            if (!validSlot(static_cast<std::uint16_t>(regOff + bc.result)) ||
                !validSlot(static_cast<std::uint16_t>(regOff + bc.operands[0])) ||
                !validSlot(static_cast<std::uint16_t>(regOff + bc.operands[1]))) {
                return false;
            }
            const auto t0 = bcFn.registerTypes[bc.operands[0]];
            const auto t1 = bcFn.registerTypes[bc.operands[1]];
            const auto okEqType = [](semantic::PrimitiveType t) noexcept {
                return t == semantic::PrimitiveType::Int ||
                       t == semantic::PrimitiveType::UInt ||
                       t == semantic::PrimitiveType::Long ||
                       t == semantic::PrimitiveType::ULong ||
                       t == semantic::PrimitiveType::Byte ||
                       t == semantic::PrimitiveType::SByte ||
                       t == semantic::PrimitiveType::Short ||
                       t == semantic::PrimitiveType::UShort ||
                       t == semantic::PrimitiveType::Char ||
                       t == semantic::PrimitiveType::Double ||
                       t == semantic::PrimitiveType::Float ||
                       t == semantic::PrimitiveType::Bool ||
                       t == semantic::PrimitiveType::Enum ||
                       t == semantic::PrimitiveType::Struct ||
                       t == semantic::PrimitiveType::Object ||
                       t == semantic::PrimitiveType::Array ||
                       t == semantic::PrimitiveType::String ||
                       t == semantic::PrimitiveType::Null;
            };
            return okEqType(t0) && okEqType(t1);
        }
        default:
            return false;
        }
    }
    case FO::InlineCallEnter:
        return validSlot(fi.dstSlot) && fi.inlineArgCount <= 4u;
    case FO::InlineReturnVoid:
        return true;
    case FO::InlineReturnVoidJump:
        return fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::InlineReturnScalar:
        return validSlot(fi.src0Slot) &&
               (fi.dstSlot == runtime::detail::InvalidCompactReg || validSlot(fi.dstSlot));
    case FO::InlineReturnScalarJump:
        return validSlot(fi.src0Slot) &&
               (fi.dstSlot == runtime::detail::InvalidCompactReg || validSlot(fi.dstSlot)) &&
               fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::TermJumpNoArgs:
        return fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::TermBranchBoolNoArgs:
        return validSlot(fi.src0Slot) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::TermBranchGeneric:
        return fi.trueTargetOffset < execFunc.flatStream.size() &&
               (fi.falseTargetOffset == runtime::detail::InvalidStreamOffset ||
                fi.falseTargetOffset < execFunc.flatStream.size());
    case FO::TermReturnVoid:
        return true;
    case FO::TermReturnScalarFast:
        return validSlot(fi.src0Slot);
    case FO::TermReturnGeneric:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isFunctionEligibleForStencilJit(
    const runtime::detail::ExecutionFunction& execFunc) noexcept {
    if (!execFunc.canUseFlatFastPath ||
        execFunc.flatStream.empty() ||
        execFunc.hasExceptionHandlers ||
        execFunc.isAsyncFunction ||
        execFunc.directNativeReference != nullptr ||
        execFunc.bytecode == nullptr ||
        execFunc.entryStreamOffset >= execFunc.flatStream.size() ||
        execFunc.totalFrameSlots == 0 ||
        execFunc.totalFrameSlots > 4096) {
        return false;
    }
    for (const auto& fi : execFunc.flatStream) {
        if (!isStencilSupportedFlatOp(execFunc, fi)) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Shared Cold-Trap Metadata & Native Machine Code Emitters (x86_64 + ARM64)
// ============================================================================

struct ColdTrapStub {
    enum class Kind : std::uint8_t {
        BudgetSimple,
        BudgetPartialSuper,
        IntegerOverflow,
        DivisionByZero,
        TypeMismatch,
        ArrayLengthSlow,
        ArrayLengthBranchSlow,
        ArrayLoadSlow,
        ArrayStoreSlow,
        FlatStructSlow,
    };

    Kind kind = Kind::BudgetSimple;
    std::uint32_t stubLabel = 0;
    std::uint32_t resumeLabel = 0;
    std::uint32_t altResumeLabel = 0;
    std::uint32_t flatIdx = 0;
    std::int32_t restoreBudget = 0;
};

#if UINTPTR_MAX == 0xffffffffffffffffULL

enum class X64Reg : std::uint8_t {
    RAX = 0,
    RCX = 1,
    RDX = 2,
    RBX = 3,
    RSP = 4,
    RBP = 5,
    RSI = 6,
    RDI = 7,
    R8  = 8,
    R9  = 9,
    R10 = 10,
    R11 = 11,
    R12 = 12,
    R13 = 13,
    R14 = 14,
    R15 = 15,
};

enum class X64Cond : std::uint8_t {
    O  = 0x0, // jo
    NO = 0x1, // jno
    B  = 0x2, // jb / jc (unsigned <)
    AE = 0x3, // jae / jnc (unsigned >=)
    E  = 0x4, // je / jz
    NE = 0x5, // jne / jnz
    BE = 0x6, // jbe (unsigned <=)
    A  = 0x7, // ja (unsigned >)
    P  = 0xA, // jp (parity / NaN unordered)
    NP = 0xB, // jnp
    L  = 0xC, // jl (signed <)
    GE = 0xD, // jge (signed >=)
    LE = 0xE, // jle (signed <=)
    G  = 0xF, // jg (signed >)
};

[[nodiscard]] constexpr X64Cond invertCond(X64Cond cc) noexcept {
    return static_cast<X64Cond>(static_cast<std::uint8_t>(cc) ^ 1u);
}

[[nodiscard]] constexpr X64Cond quickCmpToCond(runtime::detail::QuickOpcode op) noexcept {
    using QO = runtime::detail::QuickOpcode;
    switch (op) {
    case QO::LessInt: return X64Cond::L;
    case QO::LessOrEqualInt: return X64Cond::LE;
    case QO::GreaterInt: return X64Cond::G;
    case QO::GreaterOrEqualInt: return X64Cond::GE;
    case QO::EqualInt: return X64Cond::E;
    case QO::NotEqualInt: return X64Cond::NE;
    default: return X64Cond::E;
    }
}

class X64Emitter {
public:
    struct Label {
        std::size_t boundOffset = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> patches;
        [[nodiscard]] bool bound() const noexcept {
            return boundOffset != std::numeric_limits<std::size_t>::max();
        }
    };

    std::uint32_t createLabel() {
        const auto id = static_cast<std::uint32_t>(labels_.size());
        labels_.emplace_back();
        return id;
    }

    void bind(std::uint32_t labelId) {
        auto& lbl = labels_[labelId];
        lbl.boundOffset = buf_.size();
        for (const std::size_t patchPos : lbl.patches) {
            patchRel32(patchPos, lbl.boundOffset);
        }
        lbl.patches.clear();
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

    void emit8(std::uint8_t b) { buf_.push_back(b); }
    void emit32(std::uint32_t v) {
        std::uint8_t tmp[4];
        std::memcpy(tmp, &v, 4);
        buf_.insert(buf_.end(), tmp, tmp + 4);
    }
    void emit64(std::uint64_t v) {
        std::uint8_t tmp[8];
        std::memcpy(tmp, &v, 8);
        buf_.insert(buf_.end(), tmp, tmp + 8);
    }

    void emitRex(bool w, std::uint8_t reg, std::uint8_t idx, std::uint8_t rm) {
        const std::uint8_t rex = static_cast<std::uint8_t>(
            0x40u |
            (w ? 0x08u : 0u) |
            ((reg >= 8u) ? 0x04u : 0u) |
            ((idx >= 8u) ? 0x02u : 0u) |
            ((rm >= 8u) ? 0x01u : 0u));
        if (rex != 0x40u) emit8(rex);
    }

    void emitModRMReg(std::uint8_t reg, std::uint8_t rm) {
        emit8(static_cast<std::uint8_t>((3u << 6) | ((reg & 7u) << 3) | (rm & 7u)));
    }

    void emitMem(std::uint8_t regOrDigit, X64Reg base, std::int32_t disp) {
        const std::uint8_t b = static_cast<std::uint8_t>(base) & 7u;
        const std::uint8_t r = regOrDigit & 7u;
        if (disp == 0 && b != 5u) {
            emit8(static_cast<std::uint8_t>((0u << 6) | (r << 3) | b));
            if (b == 4u) emit8(0x24u);
        } else if (disp >= -128 && disp <= 127) {
            emit8(static_cast<std::uint8_t>((1u << 6) | (r << 3) | b));
            if (b == 4u) emit8(0x24u);
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(disp)));
        } else {
            emit8(static_cast<std::uint8_t>((2u << 6) | (r << 3) | b));
            if (b == 4u) emit8(0x24u);
            emit32(static_cast<std::uint32_t>(disp));
        }
    }

    // Indexed SIB [base + index * scale], index != RSP/R12, disp == 0
    void emitSibMem0(std::uint8_t reg, X64Reg base, X64Reg index, std::uint8_t scaleShift) {
        const std::uint8_t b = static_cast<std::uint8_t>(base) & 7u;
        const std::uint8_t ix = static_cast<std::uint8_t>(index) & 7u;
        if (b == 5u) {
            emit8(static_cast<std::uint8_t>((1u << 6) | ((reg & 7u) << 3) | 4u));
            emit8(static_cast<std::uint8_t>(((scaleShift & 3u) << 6) | (ix << 3) | b));
            emit8(0x00u);
        } else {
            emit8(static_cast<std::uint8_t>((0u << 6) | ((reg & 7u) << 3) | 4u));
            emit8(static_cast<std::uint8_t>(((scaleShift & 3u) << 6) | (ix << 3) | b));
        }
    }

    void push(X64Reg r) {
        const auto rv = static_cast<std::uint8_t>(r);
        if (rv >= 8u) emit8(0x41u);
        emit8(static_cast<std::uint8_t>(0x50u + (rv & 7u)));
    }

    void pop(X64Reg r) {
        const auto rv = static_cast<std::uint8_t>(r);
        if (rv >= 8u) emit8(0x41u);
        emit8(static_cast<std::uint8_t>(0x58u + (rv & 7u)));
    }

    void ret() { emit8(0xC3u); }

    void movReg64(X64Reg dst, X64Reg src) {
        emitRex(true, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(dst));
        emit8(0x89u);
        emitModRMReg(static_cast<std::uint8_t>(src), static_cast<std::uint8_t>(dst));
    }

    void xorReg32(X64Reg dst, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(dst));
        emit8(0x31u);
        emitModRMReg(static_cast<std::uint8_t>(src), static_cast<std::uint8_t>(dst));
    }

    void movImm32(X64Reg dst, std::uint32_t imm) {
        const auto dv = static_cast<std::uint8_t>(dst);
        if (imm == 0u) {
            xorReg32(dst, dst);
            return;
        }
        if (dv >= 8u) emit8(0x41u);
        emit8(static_cast<std::uint8_t>(0xB8u + (dv & 7u)));
        emit32(imm);
    }

    void movImm64(X64Reg dst, std::uint64_t imm) {
        if (imm == 0u) {
            xorReg32(dst, dst);
            return;
        }
        if (imm <= 0x7fffffffULL) {
            movImm32(dst, static_cast<std::uint32_t>(imm));
            return;
        }
        const auto s64 = static_cast<std::int64_t>(imm);
        if (fitsInt32(s64)) {
            emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
            emit8(0xC7u);
            emitModRMReg(0, static_cast<std::uint8_t>(dst));
            emit32(static_cast<std::uint32_t>(static_cast<std::int32_t>(s64)));
            return;
        }
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(static_cast<std::uint8_t>(0xB8u + (static_cast<std::uint8_t>(dst) & 7u)));
        emit64(imm);
    }

    void loadMem64(X64Reg dst, X64Reg base, std::int32_t disp) {
        emitRex(true, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(base));
        emit8(0x8Bu);
        emitMem(static_cast<std::uint8_t>(dst), base, disp);
    }

    void storeMem64(X64Reg base, std::int32_t disp, X64Reg src) {
        emitRex(true, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(base));
        emit8(0x89u);
        emitMem(static_cast<std::uint8_t>(src), base, disp);
    }

    void storeMem64Imm32SignExt(X64Reg base, std::int32_t disp, std::int32_t imm) {
        emitRex(true, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0xC7u);
        emitMem(0, base, disp);
        emit32(static_cast<std::uint32_t>(imm));
    }

    void loadMem32(X64Reg dst, X64Reg base, std::int32_t disp) {
        emitRex(false, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(base));
        emit8(0x8Bu);
        emitMem(static_cast<std::uint8_t>(dst), base, disp);
    }

    void storeMem32(X64Reg base, std::int32_t disp, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(base));
        emit8(0x89u);
        emitMem(static_cast<std::uint8_t>(src), base, disp);
    }

    void storeMem32Imm(X64Reg base, std::int32_t disp, std::uint32_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0xC7u);
        emitMem(0, base, disp);
        emit32(imm);
    }

    void loadMem8ZeroExt(X64Reg dst, X64Reg base, std::int32_t disp) {
        emitRex(false, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0xB6u);
        emitMem(static_cast<std::uint8_t>(dst), base, disp);
    }

    // Store low 8-bit of RAX/RCX/RDX/RBX (codes 0..3)
    void storeMem8Reg(X64Reg base, std::int32_t disp, X64Reg byteSrc0To3) {
        emitRex(false, static_cast<std::uint8_t>(byteSrc0To3), 0, static_cast<std::uint8_t>(base));
        emit8(0x88u);
        emitMem(static_cast<std::uint8_t>(byteSrc0To3), base, disp);
    }

    void storeMem8Imm(X64Reg base, std::int32_t disp, std::uint8_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0xC6u);
        emitMem(0, base, disp);
        emit8(imm);
    }

    void cmpMem8Imm(X64Reg base, std::int32_t disp, std::uint8_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0x80u);
        emitMem(7, base, disp);
        emit8(imm);
    }

    void movsxd64From32(X64Reg dst64, X64Reg src32) {
        emitRex(true, static_cast<std::uint8_t>(dst64), 0, static_cast<std::uint8_t>(src32));
        emit8(0x63u);
        emitModRMReg(static_cast<std::uint8_t>(dst64), static_cast<std::uint8_t>(src32));
    }

    void addImm64(X64Reg dst, std::int32_t imm) {
        if (imm == 0) return;
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        if (imm >= -128 && imm <= 127) {
            emit8(0x83u);
            emitModRMReg(0, static_cast<std::uint8_t>(dst));
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x81u);
            emitModRMReg(0, static_cast<std::uint8_t>(dst));
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void subImm64(X64Reg dst, std::int32_t imm) {
        if (imm == 0) return;
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        if (imm >= -128 && imm <= 127) {
            emit8(0x83u);
            emitModRMReg(5, static_cast<std::uint8_t>(dst));
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x81u);
            emitModRMReg(5, static_cast<std::uint8_t>(dst));
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void inc64(X64Reg dst) {
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0xFFu);
        emitModRMReg(0, static_cast<std::uint8_t>(dst));
    }

    void addReg32(X64Reg dst, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(dst));
        emit8(0x01u);
        emitModRMReg(static_cast<std::uint8_t>(src), static_cast<std::uint8_t>(dst));
    }

    void subReg32(X64Reg dst, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(src), 0, static_cast<std::uint8_t>(dst));
        emit8(0x29u);
        emitModRMReg(static_cast<std::uint8_t>(src), static_cast<std::uint8_t>(dst));
    }

    void imulReg32(X64Reg dst, X64Reg src) {
        emitRex(false, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(src));
        emit8(0x0Fu);
        emit8(0xAFu);
        emitModRMReg(static_cast<std::uint8_t>(dst), static_cast<std::uint8_t>(src));
    }

    void negReg32(X64Reg dst) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0xF7u);
        emitModRMReg(3, static_cast<std::uint8_t>(dst));
    }

    void cdq() { emit8(0x99u); }

    void idivReg32(X64Reg divisor) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(divisor));
        emit8(0xF7u);
        emitModRMReg(7, static_cast<std::uint8_t>(divisor));
    }

    void testReg32(X64Reg a, X64Reg b) {
        emitRex(false, static_cast<std::uint8_t>(b), 0, static_cast<std::uint8_t>(a));
        emit8(0x85u);
        emitModRMReg(static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a));
    }

    void cmpReg32(X64Reg lhs, X64Reg rhs) {
        emitRex(false, static_cast<std::uint8_t>(rhs), 0, static_cast<std::uint8_t>(lhs));
        emit8(0x39u);
        emitModRMReg(static_cast<std::uint8_t>(rhs), static_cast<std::uint8_t>(lhs));
    }

    void cmpReg64(X64Reg lhs, X64Reg rhs) {
        emitRex(true, static_cast<std::uint8_t>(rhs), 0, static_cast<std::uint8_t>(lhs));
        emit8(0x39u);
        emitModRMReg(static_cast<std::uint8_t>(rhs), static_cast<std::uint8_t>(lhs));
    }

    void cmpReg32Imm(X64Reg lhs, std::int32_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(lhs));
        if (imm >= -128 && imm <= 127) {
            emit8(0x83u);
            emitModRMReg(7, static_cast<std::uint8_t>(lhs));
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x81u);
            emitModRMReg(7, static_cast<std::uint8_t>(lhs));
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void cmpReg64Mem(X64Reg lhs, X64Reg base, std::int32_t disp) {
        emitRex(true, static_cast<std::uint8_t>(lhs), 0, static_cast<std::uint8_t>(base));
        emit8(0x3Bu); // CMP r64, r/m64 (lhs - [base+disp])
        emitMem(static_cast<std::uint8_t>(lhs), base, disp);
    }

    void cmpReg32Mem(X64Reg lhs, X64Reg base, std::int32_t disp) {
        emitRex(false, static_cast<std::uint8_t>(lhs), 0, static_cast<std::uint8_t>(base));
        emit8(0x3Bu); // CMP r32, r/m32 (lhs - [base+disp])
        emitMem(static_cast<std::uint8_t>(lhs), base, disp);
    }

    void cmpMem64Imm32(X64Reg base, std::int32_t disp, std::int32_t imm) {
        emitRex(true, 0, 0, static_cast<std::uint8_t>(base));
        if (imm >= -128 && imm <= 127) {
            emit8(0x83u);
            emitMem(7, base, disp);
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x81u);
            emitMem(7, base, disp);
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void setccAl(X64Cond cc) {
        emit8(0x0Fu);
        emit8(static_cast<std::uint8_t>(0x90u + static_cast<std::uint8_t>(cc)));
        emit8(0xC0u); // AL
    }

    void setccCl(X64Cond cc) {
        emit8(0x0Fu);
        emit8(static_cast<std::uint8_t>(0x90u + static_cast<std::uint8_t>(cc)));
        emit8(0xC1u); // CL
    }

    // SSE / XMM instructions (mandatory prefix BEFORE REX!)
    void movupsLoad(std::uint8_t xmmDst, X64Reg base, std::int32_t disp) {
        emitRex(false, xmmDst, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x10u);
        emitMem(xmmDst, base, disp);
    }

    void movupsStore(X64Reg base, std::int32_t disp, std::uint8_t xmmSrc) {
        emitRex(false, xmmSrc, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x11u);
        emitMem(xmmSrc, base, disp);
    }

    void movupsLoadSib1(std::uint8_t xmmDst, X64Reg base, X64Reg idx) {
        emitRex(false, xmmDst, static_cast<std::uint8_t>(idx), static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x10u);
        emitSibMem0(xmmDst, base, idx, 0);
    }

    void movupsStoreSib1(X64Reg base, X64Reg idx, std::uint8_t xmmSrc) {
        emitRex(false, xmmSrc, static_cast<std::uint8_t>(idx), static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x11u);
        emitSibMem0(xmmSrc, base, idx, 0);
    }

    void leaDisp64(X64Reg dst, X64Reg base, std::int32_t disp) {
        emitRex(true, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(base));
        emit8(0x8Du);
        emitMem(static_cast<std::uint8_t>(dst), base, disp);
    }

    void leaSib1_64(X64Reg dst, X64Reg base, X64Reg idx) {
        emitRex(true, static_cast<std::uint8_t>(dst), static_cast<std::uint8_t>(idx), static_cast<std::uint8_t>(base));
        emit8(0x8Du);
        emitSibMem0(static_cast<std::uint8_t>(dst), base, idx, 0);
    }

    void shlImm64(X64Reg dst, std::uint8_t shift) {
        emitRex(true, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0xC1u);
        emitModRMReg(4, static_cast<std::uint8_t>(dst));
        emit8(shift);
    }

    void shrImm32(X64Reg dst, std::uint8_t shift) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0xC1u);
        emitModRMReg(5, static_cast<std::uint8_t>(dst));
        emit8(shift);
    }

    void andImm32(X64Reg dst, std::uint32_t mask) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0x81u);
        emitModRMReg(4, static_cast<std::uint8_t>(dst));
        emit32(mask);
    }

    void orImm32(X64Reg dst, std::uint8_t imm) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(dst));
        emit8(0x83u);
        emitModRMReg(1, static_cast<std::uint8_t>(dst));
        emit8(imm);
    }

    void imulImm32_64(X64Reg dst, X64Reg src, std::int32_t imm) {
        emitRex(true, static_cast<std::uint8_t>(dst), 0, static_cast<std::uint8_t>(src));
        if (imm >= -128 && imm <= 127) {
            emit8(0x6Bu);
            emitModRMReg(static_cast<std::uint8_t>(dst), static_cast<std::uint8_t>(src));
            emit8(static_cast<std::uint8_t>(static_cast<std::int8_t>(imm)));
        } else {
            emit8(0x69u);
            emitModRMReg(static_cast<std::uint8_t>(dst), static_cast<std::uint8_t>(src));
            emit32(static_cast<std::uint32_t>(imm));
        }
    }

    void cmpMem16Imm(X64Reg base, std::int32_t disp, std::uint16_t imm) {
        emit8(0x66u);
        emitRex(false, 0, 0, static_cast<std::uint8_t>(base));
        emit8(0x81u);
        emitMem(7, base, disp);
        emit8(static_cast<std::uint8_t>(imm & 0xFFu));
        emit8(static_cast<std::uint8_t>((imm >> 8u) & 0xFFu));
    }

    void repMovsq() {
        emit8(0xF3u);
        emit8(0x48u);
        emit8(0xA5u);
    }

    void xorpsSelf(std::uint8_t xmm) {
        emit8(0x0Fu);
        emit8(0x57u);
        emitModRMReg(xmm, xmm);
    }

    void movsdLoad(std::uint8_t xmmDst, X64Reg base, std::int32_t disp) {
        emit8(0xF2u);
        emitRex(false, xmmDst, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x10u);
        emitMem(xmmDst, base, disp);
    }

    void movsdStore(X64Reg base, std::int32_t disp, std::uint8_t xmmSrc) {
        emit8(0xF2u);
        emitRex(false, xmmSrc, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x11u);
        emitMem(xmmSrc, base, disp);
    }

    void movssLoad(std::uint8_t xmmDst, X64Reg base, std::int32_t disp) {
        emit8(0xF3u);
        emitRex(false, xmmDst, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x10u);
        emitMem(xmmDst, base, disp);
    }

    void movssStore(X64Reg base, std::int32_t disp, std::uint8_t xmmSrc) {
        emit8(0xF3u);
        emitRex(false, xmmSrc, 0, static_cast<std::uint8_t>(base));
        emit8(0x0Fu);
        emit8(0x11u);
        emitMem(xmmSrc, base, disp);
    }

    void movqToXmm(std::uint8_t xmmDst, X64Reg src64) {
        emit8(0x66u);
        emitRex(true, xmmDst, 0, static_cast<std::uint8_t>(src64));
        emit8(0x0Fu);
        emit8(0x6Eu);
        emitModRMReg(xmmDst, static_cast<std::uint8_t>(src64));
    }

    void binarySd(std::uint8_t opByte, std::uint8_t xmmDst, std::uint8_t xmmSrc) {
        emit8(0xF2u);
        emit8(0x0Fu);
        emit8(opByte);
        emitModRMReg(xmmDst, xmmSrc);
    }

    void binarySs(std::uint8_t opByte, std::uint8_t xmmDst, std::uint8_t xmmSrc) {
        emit8(0xF3u);
        emit8(0x0Fu);
        emit8(opByte);
        emitModRMReg(xmmDst, xmmSrc);
    }

    void ucomisd(std::uint8_t xmmLhs, std::uint8_t xmmRhs) {
        emit8(0x66u);
        emit8(0x0Fu);
        emit8(0x2Eu);
        emitModRMReg(xmmLhs, xmmRhs);
    }

    void ucomiss(std::uint8_t xmmLhs, std::uint8_t xmmRhs) {
        emit8(0x0Fu);
        emit8(0x2Eu);
        emitModRMReg(xmmLhs, xmmRhs);
    }

    void cvtsi2sd64(std::uint8_t xmmDst, X64Reg src64) {
        xorpsSelf(xmmDst);
        emit8(0xF2u);
        emitRex(true, xmmDst, 0, static_cast<std::uint8_t>(src64));
        emit8(0x0Fu);
        emit8(0x2Au);
        emitModRMReg(xmmDst, static_cast<std::uint8_t>(src64));
    }

    void cvtsi2ss64(std::uint8_t xmmDst, X64Reg src64) {
        xorpsSelf(xmmDst);
        emit8(0xF3u);
        emitRex(true, xmmDst, 0, static_cast<std::uint8_t>(src64));
        emit8(0x0Fu);
        emit8(0x2Au);
        emitModRMReg(xmmDst, static_cast<std::uint8_t>(src64));
    }

    void cvtss2sd(std::uint8_t xmmDst, std::uint8_t xmmSrc) {
        emit8(0xF3u);
        emit8(0x0Fu);
        emit8(0x5Au);
        emitModRMReg(xmmDst, xmmSrc);
    }

    void cvtsd2ss(std::uint8_t xmmDst, std::uint8_t xmmSrc) {
        emit8(0xF2u);
        emit8(0x0Fu);
        emit8(0x5Au);
        emitModRMReg(xmmDst, xmmSrc);
    }

    void callReg(X64Reg target) {
        emitRex(false, 0, 0, static_cast<std::uint8_t>(target));
        emit8(0xFFu);
        emitModRMReg(2, static_cast<std::uint8_t>(target));
    }

    void jmp(std::uint32_t labelId) {
        emit8(0xE9u);
        emitBranchDisplacement(labelId);
    }

    void jcc(X64Cond cc, std::uint32_t labelId) {
        emit8(0x0Fu);
        emit8(static_cast<std::uint8_t>(0x80u + static_cast<std::uint8_t>(cc)));
        emitBranchDisplacement(labelId);
    }

    [[nodiscard]] bool allLabelsBound() const noexcept {
        for (const auto& lbl : labels_) {
            if (!lbl.patches.empty() && !lbl.bound()) return false;
        }
        return true;
    }

private:
    void emitBranchDisplacement(std::uint32_t labelId) {
        const std::size_t patchPos = buf_.size();
        emit32(0u);
        auto& lbl = labels_[labelId];
        if (lbl.bound()) {
            patchRel32(patchPos, lbl.boundOffset);
        } else {
            lbl.patches.push_back(patchPos);
        }
    }

    void patchRel32(std::size_t patchPos, std::size_t targetOffset) {
        const std::ptrdiff_t rel =
            static_cast<std::ptrdiff_t>(targetOffset) -
            static_cast<std::ptrdiff_t>(patchPos + 4u);
        const auto rel32 = static_cast<std::int32_t>(rel);
        std::memcpy(buf_.data() + patchPos, &rel32, 4);
    }

    std::vector<std::uint8_t> buf_;
    std::vector<Label> labels_;
};

[[maybe_unused]] [[nodiscard]] std::vector<std::uint8_t> compileFunctionX64(
    const CompiledFunctionRecord& record) {
    const auto& execFunc = *record.execFunc;
    const std::size_t nInst = execFunc.flatStream.size();

    X64Emitter as;
    std::vector<std::uint32_t> instLabels(nInst);
    for (std::size_t i = 0; i < nInst; ++i) {
        instLabels[i] = as.createLabel();
    }

    const std::uint32_t epilogueOk = as.createLabel();
    const std::uint32_t epilogueBudget = as.createLabel();
    const std::uint32_t epilogueOverflow = as.createLabel();
    const std::uint32_t epilogueDivZero = as.createLabel();
    const std::uint32_t epilogueTypeErr = as.createLabel();
    const std::uint32_t epilogueHelperErr = as.createLabel();
    const std::uint32_t sharedPopRet = as.createLabel();

    std::vector<ColdTrapStub> coldStubs;
    coldStubs.reserve(nInst);

    const auto addColdStub = [&](
        ColdTrapStub::Kind kind,
        std::uint32_t flatIdx,
        std::int32_t restoreBudget = 0,
        std::uint32_t resumeLabel = 0,
        std::uint32_t altResumeLabel = 0) {
        const std::uint32_t lbl = as.createLabel();
        coldStubs.push_back(ColdTrapStub{
            kind, lbl, resumeLabel, altResumeLabel, flatIdx, restoreBudget
        });
        return lbl;
    };

    // Function Prologue (16-byte stack aligned for both SysV AMD64 & Win64 ABIs)
    as.push(X64Reg::RBP);
    as.movReg64(X64Reg::RBP, X64Reg::RSP);
    as.push(X64Reg::RBX);
    as.push(X64Reg::RSI);
    as.push(X64Reg::RDI);
    as.push(X64Reg::R12);
    as.push(X64Reg::R13);
    as.push(X64Reg::R14);
    as.push(X64Reg::R15);
    as.subImm64(X64Reg::RSP, 40);

#if defined(_WIN32)
    as.movReg64(X64Reg::R12, X64Reg::RCX);
#else
    as.movReg64(X64Reg::R12, X64Reg::RDI);
#endif
    as.loadMem64(X64Reg::RBX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, frameBase)));
    as.loadMem64(X64Reg::R13, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)));
    as.loadMem64(X64Reg::R14, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)));

    if (execFunc.entryStreamOffset != 0) {
        as.jmp(instLabels[execFunc.entryStreamOffset]);
    }

    const auto slotOff = [](std::uint16_t slot) noexcept -> std::int32_t {
        return static_cast<std::int32_t>(slot) * 16;
    };

    const auto emitBudgetDeduct = [&](std::uint32_t idx, std::uint8_t span, bool mayNeedPartialStep) {
        as.subImm64(X64Reg::R13, static_cast<std::int32_t>(span));
        if (span > 1u && mayNeedPartialStep) {
            const auto stub = addColdStub(
                ColdTrapStub::Kind::BudgetPartialSuper, idx, static_cast<std::int32_t>(span));
            as.jcc(X64Cond::B, stub);
        } else {
            const auto stub = addColdStub(ColdTrapStub::Kind::BudgetSimple, idx);
            as.jcc(X64Cond::B, stub);
        }
    };

    const auto emitCheckTagOrTrap = [&](
        std::uint16_t slot,
        std::uint8_t expectedTag,
        std::uint32_t idx,
        std::int32_t restoreBudgetAfterFail) {
        as.cmpMem8Imm(X64Reg::RBX, slotOff(slot) + 15, expectedTag);
        const auto stub = addColdStub(ColdTrapStub::Kind::TypeMismatch, idx, restoreBudgetAfterFail);
        as.jcc(X64Cond::NE, stub);
    };

    const auto emitHelperCallInline = [&](
        std::uint32_t (*helperFn)(JitAbiContext*, std::uint32_t),
        std::uint32_t flatIdx) {
        as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
        as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
#if defined(_WIN32)
        as.movReg64(X64Reg::RCX, X64Reg::R12);
        as.movImm32(X64Reg::RDX, flatIdx);
#else
        as.movReg64(X64Reg::RDI, X64Reg::R12);
        as.movImm32(X64Reg::RSI, flatIdx);
#endif
        as.movImm64(X64Reg::RAX, fnPtrAddress(helperFn));
        as.callReg(X64Reg::RAX);
        as.loadMem64(X64Reg::RBX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, frameBase)));
        as.loadMem64(X64Reg::R13, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)));
        as.loadMem64(X64Reg::R14, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)));
        as.testReg32(X64Reg::RAX, X64Reg::RAX);
        as.jcc(X64Cond::NE, epilogueHelperErr);
    };

    // Emits integer ALU/comparison on EAX (lhs) and ECX (rhs).
    // Returns output tag (kTagInt64 -> result in RAX; kTagBool -> result 0/1 in AL).
    const auto emitQuickIntOpCore = [&](
        runtime::detail::QuickOpcode qop,
        bool checked,
        std::uint32_t idx,
        std::int32_t unexecAfterTrap) -> std::uint8_t {
        using QO = runtime::detail::QuickOpcode;
        switch (qop) {
        case QO::AddInt:
            as.addReg32(X64Reg::RAX, X64Reg::RCX);
            if (checked) {
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.jcc(X64Cond::O, ovfStub);
            }
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            return kTagInt64;
        case QO::SubtractInt:
            as.subReg32(X64Reg::RAX, X64Reg::RCX);
            if (checked) {
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.jcc(X64Cond::O, ovfStub);
            }
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            return kTagInt64;
        case QO::MultiplyInt:
            as.imulReg32(X64Reg::RAX, X64Reg::RCX);
            if (checked) {
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.jcc(X64Cond::O, ovfStub);
            }
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            return kTagInt64;
        case QO::DivideInt: {
            as.testReg32(X64Reg::RCX, X64Reg::RCX);
            const auto div0Stub = addColdStub(ColdTrapStub::Kind::DivisionByZero, idx, unexecAfterTrap);
            as.jcc(X64Cond::E, div0Stub);
            const std::uint32_t doDiv = as.createLabel();
            as.cmpReg32Imm(X64Reg::RAX, std::numeric_limits<std::int32_t>::min());
            as.jcc(X64Cond::NE, doDiv);
            as.cmpReg32Imm(X64Reg::RCX, -1);
            const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
            as.jcc(X64Cond::E, ovfStub);
            as.bind(doDiv);
            as.cdq();
            as.idivReg32(X64Reg::RCX);
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            return kTagInt64;
        }
        case QO::RemainderInt: {
            as.testReg32(X64Reg::RCX, X64Reg::RCX);
            const auto div0Stub = addColdStub(ColdTrapStub::Kind::DivisionByZero, idx, unexecAfterTrap);
            as.jcc(X64Cond::E, div0Stub);
            const std::uint32_t doRem = as.createLabel();
            const std::uint32_t remDone = as.createLabel();
            as.cmpReg32Imm(X64Reg::RAX, std::numeric_limits<std::int32_t>::min());
            as.jcc(X64Cond::NE, doRem);
            as.cmpReg32Imm(X64Reg::RCX, -1);
            as.jcc(X64Cond::NE, doRem);
            as.xorReg32(X64Reg::RAX, X64Reg::RAX);
            as.jmp(remDone);
            as.bind(doRem);
            as.cdq();
            as.idivReg32(X64Reg::RCX);
            as.movsxd64From32(X64Reg::RAX, X64Reg::RDX);
            as.bind(remDone);
            return kTagInt64;
        }
        default: {
            as.cmpReg32(X64Reg::RAX, X64Reg::RCX);
            as.setccAl(quickCmpToCond(qop));
            return kTagBool;
        }
        }
    };

    const auto emitWriteTaggedResult = [&](std::uint16_t slot, std::uint8_t tag) {
        const std::int32_t off = slotOff(slot);
        if (tag == kTagBool) {
            as.storeMem8Reg(X64Reg::RBX, off, X64Reg::RAX);
            as.storeMem8Imm(X64Reg::RBX, off + 15, kTagBool);
        } else {
            as.storeMem64(X64Reg::RBX, off, X64Reg::RAX);
            as.storeMem8Imm(X64Reg::RBX, off + 15, tag);
        }
    };

    const auto emitCondBranchTargets = [&](
        X64Cond trueCc,
        std::uint32_t idx,
        std::uint32_t trueOff,
        std::uint32_t falseOff) {
        if (falseOff == idx + 1u) {
            as.jcc(trueCc, instLabels[trueOff]);
        } else if (trueOff == idx + 1u) {
            as.jcc(invertCond(trueCc), instLabels[falseOff]);
        } else {
            as.jcc(trueCc, instLabels[trueOff]);
            as.jmp(instLabels[falseOff]);
        }
    };

    for (std::size_t idx = 0; idx < nInst; ++idx) {
        as.bind(instLabels[idx]);
        const auto& fi = execFunc.flatStream[idx];
        const auto i32Idx = static_cast<std::uint32_t>(idx);
        const auto unexecAfter = static_cast<std::int32_t>(record.unexecutedAfterOp[idx]);
        using FO = runtime::detail::FlatOpcode;
        using QO = runtime::detail::QuickOpcode;

        switch (fi.flatOpcode) {
        case FO::MoveSlot:
        case FO::AliasStructSlice:
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.dstSlot != fi.src0Slot) {
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
                as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            }
            break;

        case FO::StoreLocalStructFieldInPlace: {
            const auto targetLeafSlot = static_cast<std::uint32_t>(fi.auxImm32 + fi.aux0Slot);
            if (fi.aux1Slot == 0u) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                // Ensure RHS is not a FrameSlice/traceable Struct needing detach
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::E, slowStub);
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src1Slot));
                as.movupsStore(X64Reg::RBX, static_cast<std::int32_t>(targetLeafSlot * 16u), 0);
                as.bind(doneLbl);
            } else if (const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot);
                       subSc != nullptr && targetLeafSlot + subSc->flatSlotCount <= execFunc.totalFrameSlots) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 13, 0u);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 12, static_cast<std::uint8_t>(fi.aux1Slot & 0xFFu));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 14, static_cast<std::uint8_t>((fi.aux1Slot >> 8u) & 0xFFu));
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src1Slot) + 8);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.orImm32(X64Reg::RCX, 0x40u);
                as.movReg64(X64Reg::R8, X64Reg::RAX);
                as.shrImm32(X64Reg::R8, 24u);
                as.cmpReg32(X64Reg::R8, X64Reg::RCX);
                as.jcc(X64Cond::NE, slowStub);
                as.andImm32(X64Reg::RAX, 0x00FFFFFFu);
                as.loadMem64(X64Reg::R9, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.shlImm64(X64Reg::RAX, 4u);
                as.leaSib1_64(X64Reg::RSI, X64Reg::R9, X64Reg::RAX);
                as.leaDisp64(X64Reg::RDI, X64Reg::RBX, static_cast<std::int32_t>(targetLeafSlot * 16u));
                const std::uint32_t skipCopyLbl = as.createLabel();
                as.cmpReg64(X64Reg::RSI, X64Reg::RDI);
                as.jcc(X64Cond::E, skipCopyLbl);
                if (subSc->flatSlotCount <= 6u) {
                    for (std::uint16_t k = 0; k < subSc->flatSlotCount; ++k) {
                        as.movupsLoad(0, X64Reg::RSI, static_cast<std::int32_t>(k) * 16);
                        as.movupsStore(X64Reg::RDI, static_cast<std::int32_t>(k) * 16, 0);
                    }
                } else {
                    as.movImm32(X64Reg::RCX, static_cast<std::uint32_t>(subSc->flatSlotCount) * 2u);
                    as.repMovsq();
                }
                as.bind(skipCopyLbl);
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::LoadStructSliceField: {
            const std::uint16_t subSid = fi.aux1Slot;
            if (subSid == 0u || fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 13, 0u);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.orImm32(X64Reg::RCX, 0x40u);
                as.movReg64(X64Reg::R8, X64Reg::RAX);
                as.shrImm32(X64Reg::R8, 24u);
                as.cmpReg32(X64Reg::R8, X64Reg::RCX);
                as.jcc(X64Cond::NE, slowStub);
                as.andImm32(X64Reg::RAX, 0x00FFFFFFu);
                as.addImm64(X64Reg::RAX, static_cast<std::int32_t>(fi.aux0Slot));
                as.cmpReg32Imm(X64Reg::RAX, static_cast<std::int32_t>(runtime::StructStorageHandle::FrameOffsetMask));
                as.jcc(X64Cond::A, slowStub);
                as.loadMem64(X64Reg::R8, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                if (subSid == 0u) {
                    as.shlImm64(X64Reg::RAX, 4u);
                    as.movupsLoadSib1(0, X64Reg::R8, X64Reg::RAX);
                    as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
                } else {
                    // Zero-copy AliasSlice nested struct header construction!
                    as.shlImm64(X64Reg::RCX, 24u);
                    as.emitRex(false, static_cast<std::uint8_t>(X64Reg::RCX), 0, static_cast<std::uint8_t>(X64Reg::RAX));
                    as.emit8(0x09u); // OR eax, ecx
                    as.emitModRMReg(static_cast<std::uint8_t>(X64Reg::RCX), static_cast<std::uint8_t>(X64Reg::RAX));
                    as.movImm64(X64Reg::RDX, static_cast<std::uint64_t>(fi.integerImmediate));
                    as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot) + 0, X64Reg::RDX);
                    as.storeMem32(X64Reg::RBX, slotOff(fi.dstSlot) + 8, X64Reg::RAX);
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(subSid & 0xFFu));
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 13, 0u);
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((subSid >> 8u) & 0xFFu));
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                }
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::CopyStructSlice: {
            const std::uint16_t sid = fi.aux0Slot;
            const std::uint32_t dstPayOff = fi.auxImm32;
            const auto* sc = runtime::getFlatStructSchema(sid);
            if (sc != nullptr && dstPayOff + sc->flatSlotCount <= execFunc.totalFrameSlots) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 13, 0u);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.orImm32(X64Reg::RCX, 0x40u);
                as.movReg64(X64Reg::R8, X64Reg::RAX);
                as.shrImm32(X64Reg::R8, 24u);
                as.cmpReg32(X64Reg::R8, X64Reg::RCX);
                as.jcc(X64Cond::NE, slowStub);
                as.andImm32(X64Reg::RAX, 0x00FFFFFFu);
                as.loadMem32(X64Reg::R8, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, frameOffset32)));
                as.addImm64(X64Reg::R8, static_cast<std::int32_t>(dstPayOff));
                as.cmpReg32Imm(X64Reg::R8, static_cast<std::int32_t>(runtime::StructStorageHandle::FrameOffsetMask));
                as.jcc(X64Cond::A, slowStub);
                as.loadMem64(X64Reg::RDX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.movReg64(X64Reg::R9, X64Reg::RCX); // save (0x40 | bankId)
                as.shlImm64(X64Reg::RAX, 4u);
                as.leaSib1_64(X64Reg::RSI, X64Reg::RDX, X64Reg::RAX);
                as.leaDisp64(X64Reg::RDI, X64Reg::RBX, static_cast<std::int32_t>(dstPayOff * 16u));
                const std::uint32_t skipCopyLbl = as.createLabel();
                as.cmpReg64(X64Reg::RSI, X64Reg::RDI);
                as.jcc(X64Cond::E, skipCopyLbl);
                if (sc->flatSlotCount <= 6u) {
                    for (std::uint16_t k = 0; k < sc->flatSlotCount; ++k) {
                        as.movupsLoad(0, X64Reg::RSI, static_cast<std::int32_t>(k) * 16);
                        as.movupsStore(X64Reg::RDI, static_cast<std::int32_t>(k) * 16, 0);
                    }
                } else {
                    as.movImm32(X64Reg::RCX, static_cast<std::uint32_t>(sc->flatSlotCount) * 2u);
                    as.repMovsq();
                }
                as.bind(skipCopyLbl);
                as.shlImm64(X64Reg::R9, 24u);
                as.movReg64(X64Reg::RAX, X64Reg::R8);
                as.emitRex(false, static_cast<std::uint8_t>(X64Reg::R9), 0, static_cast<std::uint8_t>(X64Reg::RAX));
                as.emit8(0x09u); // OR eax, r9d
                as.emitModRMReg(static_cast<std::uint8_t>(X64Reg::R9), static_cast<std::uint8_t>(X64Reg::RAX));
                as.movImm64(X64Reg::RDX, static_cast<std::uint64_t>(sc->structTypeId));
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot) + 0, X64Reg::RDX);
                as.storeMem32(X64Reg::RBX, slotOff(fi.dstSlot) + 8, X64Reg::RAX);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 13, 0u);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::WritebackLocalStackRef:
            emitBudgetDeduct(i32Idx, fi.span, false);
            break;

        case FO::StoreStackRefStructFieldInPlace: {
            if (fi.aux1Slot == 0u) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                as.cmpReg32Imm(X64Reg::RAX, static_cast<std::int32_t>(runtime::ObjectRef::StackSlotRefHeapId));
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.cmpReg32Mem(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src0Slot) + 4);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.jcc(X64Cond::E, slowStub);
                as.loadMem64(X64Reg::R8, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 0);
                as.addImm64(X64Reg::RAX, static_cast<std::int32_t>(fi.aux0Slot));
                as.shlImm64(X64Reg::RAX, 4u);
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src1Slot));
                as.movupsStoreSib1(X64Reg::R8, X64Reg::RAX, 0);
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::InitStructSlice:
        case FO::StoreStructSliceField:
        case FO::InitStackRefCell:
        case FO::BindLocalStackRef:
        case FO::LoadStackRefCell:
        case FO::CheckNotNullFast:
        case FO::StoreStackRefCell:
        case FO::NewObjectFast:
        case FO::LoadFieldExactFast:
        case FO::LoadFieldFast:
        case FO::StoreFieldExactFast:
        case FO::StoreFieldFast:
        case FO::GenericBytecode:
            emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            break;

        case FO::SuperMoveParamStoreLocal:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.movupsStore(X64Reg::RBX, slotOff(fi.storeSlot), 0);
            break;

        case FO::SuperMoveParamStoreLocalElided:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movupsStore(X64Reg::RBX, slotOff(fi.storeSlot), 0);
            break;

        case FO::ConstantInt:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem64Imm32SignExt(
                X64Reg::RBX, slotOff(fi.dstSlot), static_cast<std::int32_t>(fi.integerImmediate));
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagInt64);
            break;

        case FO::ConstantLong:
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fitsInt32(fi.integerImmediate)) {
                as.storeMem64Imm32SignExt(
                    X64Reg::RBX, slotOff(fi.dstSlot), static_cast<std::int32_t>(fi.integerImmediate));
            } else {
                as.movImm64(X64Reg::RAX, static_cast<std::uint64_t>(fi.integerImmediate));
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RAX);
            }
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagLong);
            break;

        case FO::ConstantBool:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot), fi.integerImmediate != 0 ? 1u : 0u);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagBool);
            break;

        case FO::ConstantDouble: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            const auto uBits = static_cast<std::uint64_t>(fi.integerImmediate);
            if (uBits == 0u) {
                as.storeMem64Imm32SignExt(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            } else {
                as.movImm64(X64Reg::RAX, uBits);
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RAX);
            }
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;
        }

        case FO::SuperConstFloat: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            std::uint32_t fBits = 0;
            std::memcpy(&fBits, &fi.integerImmediate, 4);
            as.storeMem32Imm(X64Reg::RBX, slotOff(fi.dstSlot), fBits);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::SuperConstFloatStoreLocal: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            std::uint32_t fBits = 0;
            std::memcpy(&fBits, &fi.integerImmediate, 4);
            as.storeMem32Imm(X64Reg::RBX, slotOff(fi.storeSlot), fBits);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.storeSlot) + 15, kTagFloat);
            break;
        }

        case FO::AddIntChecked:
        case FO::AddIntUnchecked:
        case FO::SubIntChecked:
        case FO::SubIntUnchecked:
        case FO::MulIntChecked:
        case FO::MulIntUnchecked:
        case FO::DivInt:
        case FO::RemInt:
        case FO::LessInt:
        case FO::LessOrEqualInt:
        case FO::GreaterInt:
        case FO::GreaterOrEqualInt:
        case FO::EqualInt:
        case FO::NotEqualInt:
        case FO::AddIntTrustedChecked:
        case FO::AddIntTrustedUnchecked:
        case FO::SubIntTrustedChecked:
        case FO::SubIntTrustedUnchecked:
        case FO::MulIntTrustedChecked:
        case FO::MulIntTrustedUnchecked:
        case FO::LessIntTrusted: {
            const bool isTrusted =
                fi.flatOpcode >= FO::AddIntTrustedChecked &&
                fi.flatOpcode <= FO::LessIntTrusted;
            const bool isChecked =
                fi.flatOpcode == FO::AddIntChecked ||
                fi.flatOpcode == FO::SubIntChecked ||
                fi.flatOpcode == FO::MulIntChecked ||
                fi.flatOpcode == FO::AddIntTrustedChecked ||
                fi.flatOpcode == FO::SubIntTrustedChecked ||
                fi.flatOpcode == FO::MulIntTrustedChecked;
            const bool mayTrap =
                isChecked || fi.flatOpcode == FO::DivInt || fi.flatOpcode == FO::RemInt;

            QO qop = QO::AddInt;
            switch (fi.flatOpcode) {
            case FO::AddIntChecked: case FO::AddIntUnchecked:
            case FO::AddIntTrustedChecked: case FO::AddIntTrustedUnchecked:
                qop = QO::AddInt; break;
            case FO::SubIntChecked: case FO::SubIntUnchecked:
            case FO::SubIntTrustedChecked: case FO::SubIntTrustedUnchecked:
                qop = QO::SubtractInt; break;
            case FO::MulIntChecked: case FO::MulIntUnchecked:
            case FO::MulIntTrustedChecked: case FO::MulIntTrustedUnchecked:
                qop = QO::MultiplyInt; break;
            case FO::DivInt: qop = QO::DivideInt; break;
            case FO::RemInt: qop = QO::RemainderInt; break;
            case FO::LessInt: case FO::LessIntTrusted: qop = QO::LessInt; break;
            case FO::LessOrEqualInt: qop = QO::LessOrEqualInt; break;
            case FO::GreaterInt: qop = QO::GreaterInt; break;
            case FO::GreaterOrEqualInt: qop = QO::GreaterOrEqualInt; break;
            case FO::EqualInt: qop = QO::EqualInt; break;
            case FO::NotEqualInt: qop = QO::NotEqualInt; break;
            default: break;
            }

            emitBudgetDeduct(i32Idx, fi.span, mayTrap && fi.span > 1u);
            if (!isTrusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem32(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            const std::uint8_t outTag = emitQuickIntOpCore(qop, isChecked, i32Idx, unexecAfter);
            emitWriteTaggedResult(fi.dstSlot, outTag);
            break;
        }

        case FO::NegateIntUnchecked:
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.negReg32(X64Reg::RAX);
            as.movsxd64From32(X64Reg::RAX, X64Reg::RAX);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;

        case FO::LogicalNot:
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagBool, i32Idx, unexecAfter);
            as.loadMem8ZeroExt(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.emit8(0x34u); // xor al, 1
            as.emit8(0x01u);
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;

        case FO::SuperBinaryLocalIntConst:
        case FO::SuperBinaryLocalIntConstStore:
        case FO::SuperBinaryLocalIntConstStoreElided:
        case FO::SuperBinaryLocalIntConstStoreJump: {
            const bool isElided = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreElided);
            const bool isJump = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump);
            const bool hasStore = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStore);
            const bool trusted0 = (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;

            emitBudgetDeduct(i32Idx, fi.span, true);
            if (!trusted0) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            if (!isElided && !isJump) {
                as.movsxd64From32(X64Reg::RDX, X64Reg::RAX);
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RDX);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagInt64);
                as.storeMem64Imm32SignExt(
                    X64Reg::RBX, slotOff(fi.aux0Slot), static_cast<std::int32_t>(fi.integerImmediate));
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux0Slot) + 15, kTagInt64);
            }
            as.movImm32(X64Reg::RCX, static_cast<std::uint32_t>(static_cast<std::int32_t>(fi.integerImmediate)));
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, unexecAfter);
            if (isJump) {
                emitWriteTaggedResult(fi.dstSlot, outTag);
                const std::int32_t brInc =
                    ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2 : 1;
                as.addImm64(X64Reg::R14, brInc);
                as.jmp(instLabels[fi.trueTargetOffset]);
            } else if (isElided) {
                emitWriteTaggedResult(fi.storeSlot, outTag);
            } else {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
                if (hasStore) {
                    emitWriteTaggedResult(fi.storeSlot, outTag);
                }
            }
            break;
        }

        case FO::SuperBinaryLocalIntLocal:
        case FO::SuperBinaryLocalIntLocalStore:
        case FO::SuperBinaryLocalIntLocalStoreElided:
        case FO::SuperBinaryLocalIntLocalStoreJump:
        case FO::SuperBinaryLocalIntReg:
        case FO::SuperBinaryLocalIntRegStore:
        case FO::SuperBinaryLocalIntRegStoreElided:
        case FO::SuperBinaryLocalIntRegStoreJump: {
            const bool isLocalRhs =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocal ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump;
            const bool isElided =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreElided;
            const bool isJump =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreJump;
            const bool hasStore =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStore;
            const bool trustedBoth =
                (fi.arithFlags & (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                                  runtime::detail::FlatInstruction::FlagTrustedSrc1)) ==
                (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                 runtime::detail::FlatInstruction::FlagTrustedSrc1);

            emitBudgetDeduct(i32Idx, fi.span, true);
            if (!trustedBoth) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem32(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            if (!isElided && !isJump) {
                as.movsxd64From32(X64Reg::RDX, X64Reg::RAX);
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RDX);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagInt64);
                if (isLocalRhs) {
                    as.movsxd64From32(X64Reg::RDX, X64Reg::RCX);
                    as.storeMem64(X64Reg::RBX, slotOff(fi.aux0Slot), X64Reg::RDX);
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux0Slot) + 15, kTagInt64);
                }
            }
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, unexecAfter);
            if (isJump) {
                emitWriteTaggedResult(fi.dstSlot, outTag);
                const std::int32_t brInc =
                    ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2 : 1;
                as.addImm64(X64Reg::R14, brInc);
                as.jmp(instLabels[fi.trueTargetOffset]);
            } else if (isElided) {
                emitWriteTaggedResult(fi.storeSlot, outTag);
            } else {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
                if (hasStore) {
                    emitWriteTaggedResult(fi.storeSlot, outTag);
                }
            }
            break;
        }

        case FO::SuperBinaryLocalIntRegChainStore:
        case FO::SuperBinaryLocalIntRegChainStoreElided: {
            const bool isElided = (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStoreElided);
            emitBudgetDeduct(i32Idx, fi.span, true);
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 2);
            emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, 2);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem32(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            if (!isElided) {
                as.movsxd64From32(X64Reg::RDX, X64Reg::RAX);
                as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RDX);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagInt64);
            }
            const std::uint8_t midTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, 2);
            if (!isElided) {
                emitWriteTaggedResult(fi.aux0Slot, midTag);
            }
            emitCheckTagOrTrap(fi.src2Slot, kTagInt64, i32Idx, 1);
            as.loadMem32(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src2Slot));
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode2, fi.checked2(), i32Idx, 1);
            if (!isElided) {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
            }
            emitWriteTaggedResult(fi.storeSlot, outTag);
            break;
        }

        case FO::SuperLessLocalConstBranch: {
            const bool trusted0 = (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (!trusted0) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 1);
            }
            as.inc64(X64Reg::R14);
            as.cmpMem64Imm32(
                X64Reg::RBX, slotOff(fi.src0Slot), static_cast<std::int32_t>(fi.integerImmediate));
            emitCondBranchTargets(
                quickCmpToCond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::SuperLessLocalRegBranch: {
            const bool trustedBoth =
                (fi.arithFlags & (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                                  runtime::detail::FlatInstruction::FlagTrustedSrc1)) ==
                (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                 runtime::detail::FlatInstruction::FlagTrustedSrc1);
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (!trustedBoth) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 1);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, 1);
            }
            as.inc64(X64Reg::R14);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src1Slot));
            emitCondBranchTargets(
                quickCmpToCond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::SuperLessLocalArrayLengthBranch: {
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src1Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLengthBranchSlow,
                i32Idx,
                0,
                instLabels[fi.trueTargetOffset],
                instLabels[fi.falseTargetOffset]);

            // Verify lhs is Int64 & array hits inline cache
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagInt64);
            as.jcc(X64Cond::NE, slowStub);
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagObjectRef);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src1Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src1Slot) + 8);
            as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
            as.jcc(X64Cond::NE, slowStub);

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.inc64(X64Reg::R14);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem32(X64Reg::RCX, X64Reg::R12, cacheDisp + 24); // zero-extended uint32 length
            as.cmpReg64(X64Reg::RAX, X64Reg::RCX);
            emitCondBranchTargets(
                quickCmpToCond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::ArrayLengthFast: {
            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLengthSlow, i32Idx, 0, doneLbl);

            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
            as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
            as.jcc(X64Cond::NE, slowStub);

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem32(X64Reg::RAX, X64Reg::R12, cacheDisp + 24);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            as.bind(doneLbl);
            break;
        }

        case FO::NewArrayFast:
            emitHelperCallInline(&jitNewArrayHelper, i32Idx);
            break;

        case FO::LoadElementIntFast:
        case FO::LoadElementTypedFast: {
            const bool isTyped = (fi.flatOpcode == FO::LoadElementTypedFast);
            const auto elemType = isTyped
                ? static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1))
                : semantic::PrimitiveType::Int;
            const auto elemTid = isTyped
                ? static_cast<semantic::SymbolId>(fi.integerImmediate)
                : semantic::SymbolId{0};

            std::uint8_t fastEncoding = 0u;
            if (elemTid == 0) {
                if (elemType == semantic::PrimitiveType::Int) fastEncoding = kPackedInt32;
                else if (elemType == semantic::PrimitiveType::Double) fastEncoding = kPackedFloat64;
                else if (elemType == semantic::PrimitiveType::Bool) fastEncoding = kPackedBool;
            }

            if (fastEncoding == 0u) {
                if (isTyped &&
                    elemType == semantic::PrimitiveType::Struct &&
                    execFunc.hasStructPayloads &&
                    fi.dstSlot < execFunc.slotStructSchemaIds.size() &&
                    execFunc.slotStructSchemaIds[fi.dstSlot] != 0u) {
                    const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
                    const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
                    const auto* sc = runtime::getFlatStructSchema(sid);
                    if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg &&
                        static_cast<std::uint32_t>(payOff) + sc->flatSlotCount <= execFunc.totalFrameSlots) {
                        const std::uint32_t doneLbl = as.createLabel();
                        const std::int32_t cacheDisp =
                            static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                            static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
                        const auto slowStub = addColdStub(
                            ColdTrapStub::Kind::ArrayLoadSlow, i32Idx, 0, doneLbl);

                        // 1. Inline FlatStruct[] (isPacked == 2u) cache hit check
                        as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
                        as.jcc(X64Cond::NE, slowStub);
                        as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
                        as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
                        as.jcc(X64Cond::NE, slowStub);
                        as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                        as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
                        as.jcc(X64Cond::NE, slowStub);
                        as.cmpMem8Imm(X64Reg::R12, cacheDisp + 13, 2u);
                        as.jcc(X64Cond::NE, slowStub);
                        as.cmpMem16Imm(X64Reg::R12, cacheDisp + 30, sid);
                        as.jcc(X64Cond::NE, slowStub);

                        // 2. Index validation & bounds check
                        as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagInt64);
                        as.jcc(X64Cond::NE, slowStub);
                        as.loadMem64(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
                        as.loadMem32(X64Reg::RDX, X64Reg::R12, cacheDisp + 24);
                        as.cmpReg64(X64Reg::RCX, X64Reg::RDX);
                        as.jcc(X64Cond::AE, slowStub);

                        // 3. Direct hardware slice copy from FlatStruct[] buffer -> flat frame payload!
                        emitBudgetDeduct(i32Idx, fi.span, false);
                        as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16); // Value* data
                        as.imulImm32_64(X64Reg::RAX, X64Reg::RCX, static_cast<std::int32_t>(sc->flatSlotCount) * 16);
                        as.leaSib1_64(X64Reg::RSI, X64Reg::RDX, X64Reg::RAX);
                        as.leaDisp64(X64Reg::RDI, X64Reg::RBX, static_cast<std::int32_t>(payOff) * 16);
                        if (sc->flatSlotCount <= 6u) {
                            for (std::uint16_t k = 0; k < sc->flatSlotCount; ++k) {
                                as.movupsLoad(0, X64Reg::RSI, static_cast<std::int32_t>(k) * 16);
                                as.movupsStore(X64Reg::RDI, static_cast<std::int32_t>(k) * 16, 0);
                            }
                        } else {
                            as.movImm32(X64Reg::RCX, static_cast<std::uint32_t>(sc->flatSlotCount) * 2u);
                            as.repMovsq();
                        }
                        as.loadMem64(X64Reg::RDX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                        as.loadMem8ZeroExt(X64Reg::RCX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                        as.orImm32(X64Reg::RCX, 0x40u);
                        as.shlImm64(X64Reg::RCX, 24u);
                        as.loadMem32(X64Reg::RAX, X64Reg::RDX, static_cast<std::int32_t>(offsetof(JitRuntimeSession, frameOffset32)));
                        if (payOff != 0u) {
                            as.addImm64(X64Reg::RAX, static_cast<std::int32_t>(payOff));
                        }
                        as.emitRex(false, static_cast<std::uint8_t>(X64Reg::RCX), 0, static_cast<std::uint8_t>(X64Reg::RAX));
                        as.emit8(0x09u); // OR eax, ecx
                        as.emitModRMReg(static_cast<std::uint8_t>(X64Reg::RCX), static_cast<std::uint8_t>(X64Reg::RAX));
                        as.movImm64(X64Reg::RDX, static_cast<std::uint64_t>(elemTid));
                        as.storeMem64(X64Reg::RBX, slotOff(fi.dstSlot) + 0, X64Reg::RDX);
                        as.storeMem32(X64Reg::RBX, slotOff(fi.dstSlot) + 8, X64Reg::RAX);
                        as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                        as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 13, 0u);
                        as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                        as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                        as.bind(doneLbl);
                        break;
                    }
                }
                emitHelperCallInline(&jitLoadElementHelper, i32Idx);
                break;
            }

            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLoadSlow, i32Idx, 0, doneLbl);

            // 1. Inline PackedArrayFastView cache hit check
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
            as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
            as.jcc(X64Cond::NE, slowStub);
            as.cmpMem8Imm(X64Reg::R12, cacheDisp + 14, fastEncoding);
            as.jcc(X64Cond::NE, slowStub);

            // 2. Index validation & bounds check (unsigned cmp handles both < 0 and >= length)
            const bool noBce = (fi.arithFlags & runtime::detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u;
            const bool trustedIdx = !isTyped &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            if (!noBce && !trustedIdx) {
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagInt64);
                as.jcc(X64Cond::NE, slowStub);
            }
            as.loadMem64(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            if (!noBce) {
                as.loadMem32(X64Reg::RDX, X64Reg::R12, cacheDisp + 24);
                as.cmpReg64(X64Reg::RCX, X64Reg::RDX);
                as.jcc(X64Cond::AE, slowStub);
            }

            // 3. Exact budget deduction + direct unboxed payload read
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16); // void* rawData
            if (fastEncoding == kPackedInt32) {
                // movsxd rax, dword ptr [rdx + rcx*4]
                as.emitRex(true, 0, 0, 0);
                as.emit8(0x63u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 2);
                emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            } else if (fastEncoding == kPackedFloat64) {
                // movsd xmm0, qword ptr [rdx + rcx*8]
                as.emit8(0xF2u);
                as.emit8(0x0Fu);
                as.emit8(0x10u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 3);
                as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            } else {
                // movzx eax, byte ptr [rdx + rcx*1]
                as.emit8(0x0Fu);
                as.emit8(0xB6u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 0);
                emitWriteTaggedResult(fi.dstSlot, kTagBool);
            }
            as.bind(doneLbl);
            break;
        }

        case FO::StoreElementStructFieldInPlace: {
            const auto rootSid = static_cast<std::uint16_t>(fi.auxImm32);
            const auto* rootSc = runtime::getFlatStructSchema(rootSid);
            if (fi.aux1Slot == 0u && rootSc != nullptr && fi.aux0Slot < rootSc->flatSlotCount) {
                const std::uint32_t doneLbl = as.createLabel();
                const std::int32_t cacheDisp =
                    static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                    static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
                const auto slowStub = addColdStub(
                    ColdTrapStub::Kind::ArrayStoreSlow, i32Idx, 0, doneLbl);

                // 1. Inline FlatStruct[] (isPacked == 2u) cache hit check
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
                as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
                as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem8Imm(X64Reg::R12, cacheDisp + 13, 2u);
                as.jcc(X64Cond::NE, slowStub);
                as.cmpMem16Imm(X64Reg::R12, cacheDisp + 30, rootSid);
                as.jcc(X64Cond::NE, slowStub);

                // 2. Index validation & bounds check
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagInt64);
                as.jcc(X64Cond::NE, slowStub);
                as.loadMem64(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
                as.loadMem32(X64Reg::RDX, X64Reg::R12, cacheDisp + 24);
                as.cmpReg64(X64Reg::RCX, X64Reg::RDX);
                as.jcc(X64Cond::AE, slowStub);

                // 3. Ensure RHS is a non-traceable, non-pool, non-FrameSlice scalar (1 <= tag < FirstTraceable=23)
                as.loadMem8ZeroExt(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src2Slot) + 15);
                as.testReg32(X64Reg::RAX, X64Reg::RAX);
                as.jcc(X64Cond::E, slowStub);
                as.cmpReg32Imm(X64Reg::RAX, static_cast<std::int32_t>(runtime::ValueTag::FirstTraceable));
                as.jcc(X64Cond::AE, slowStub);

                // 4. Direct inline 128-bit XMM leaf store into FlatStruct[] element slot!
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16); // Value* data
                as.imulImm32_64(X64Reg::RAX, X64Reg::RCX, static_cast<std::int32_t>(rootSc->flatSlotCount));
                as.addImm64(X64Reg::RAX, static_cast<std::int32_t>(fi.aux0Slot));
                as.shlImm64(X64Reg::RAX, 4u);
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src2Slot));
                as.movupsStoreSib1(X64Reg::RDX, X64Reg::RAX, 0);
                as.bind(doneLbl);
                break;
            }
            emitHelperCallInline(&jitStoreElementHelper, i32Idx);
            break;
        }

        case FO::StoreElementStructFast:
        case FO::StoreElementObjFast:
            emitHelperCallInline(&jitStoreElementHelper, i32Idx);
            break;

        case FO::StoreElementIntFast:
        case FO::StoreElementPrimFast: {
            const bool isPrim = (fi.flatOpcode == FO::StoreElementPrimFast);
            const auto elemType = isPrim
                ? static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1))
                : semantic::PrimitiveType::Int;

            std::uint8_t fastEncoding = 0u;
            if (elemType == semantic::PrimitiveType::Int) fastEncoding = kPackedInt32;
            else if (elemType == semantic::PrimitiveType::Double) fastEncoding = kPackedFloat64;
            else if (elemType == semantic::PrimitiveType::Bool) fastEncoding = kPackedBool;

            if (fastEncoding == 0u) {
                emitHelperCallInline(&jitStoreElementHelper, i32Idx);
                break;
            }

            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayStoreSlow, i32Idx, 0, doneLbl);

            // 1. Inline PackedArrayFastView cache hit check
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot) + 15, kTagObjectRef);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cmpReg64Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 0);
            as.jcc(X64Cond::NE, slowStub);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot) + 8);
            as.cmpReg32Mem(X64Reg::RAX, X64Reg::R12, cacheDisp + 8);
            as.jcc(X64Cond::NE, slowStub);
            as.cmpMem8Imm(X64Reg::R12, cacheDisp + 14, fastEncoding);
            as.jcc(X64Cond::NE, slowStub);

            // 2. Index validation & bounds check
            const bool noBce = (fi.arithFlags & runtime::detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u;
            const bool trustedIdx = !isPrim &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            const bool trustedVal = !isPrim &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc1) != 0u;
            if (!noBce && !trustedIdx) {
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src1Slot) + 15, kTagInt64);
                as.jcc(X64Cond::NE, slowStub);
            }
            as.loadMem64(X64Reg::RCX, X64Reg::RBX, slotOff(fi.src1Slot));
            if (!noBce) {
                as.loadMem32(X64Reg::RDX, X64Reg::R12, cacheDisp + 24);
                as.cmpReg64(X64Reg::RCX, X64Reg::RDX);
                as.jcc(X64Cond::AE, slowStub);
            }

            // 3. Value tag & range check before mutating rawData
            if (fastEncoding == kPackedInt32) {
                if (!trustedVal) {
                    as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src2Slot) + 15, kTagInt64);
                    as.jcc(X64Cond::NE, slowStub);
                }
                as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src2Slot));
                as.movsxd64From32(X64Reg::R8, X64Reg::RAX);
                as.cmpReg64(X64Reg::RAX, X64Reg::R8);
                as.jcc(X64Cond::NE, slowStub); // >32-bit host int64 widens via slowStub!

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16);
                // mov dword ptr [rdx + rcx*4], eax
                as.emit8(0x89u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 2);
            } else if (fastEncoding == kPackedFloat64) {
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src2Slot) + 15, kTagDouble);
                as.jcc(X64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src2Slot));
                as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16);
                // movsd qword ptr [rdx + rcx*8], xmm0
                as.emit8(0xF2u);
                as.emit8(0x0Fu);
                as.emit8(0x11u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 3);
            } else {
                as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src2Slot) + 15, kTagBool);
                as.jcc(X64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem8ZeroExt(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src2Slot));
                as.loadMem64(X64Reg::RDX, X64Reg::R12, cacheDisp + 16);
                // mov byte ptr [rdx + rcx*1], al
                as.emit8(0x88u);
                as.emitSibMem0(0, X64Reg::RDX, X64Reg::RCX, 0);
            }
            as.bind(doneLbl);
            break;
        }

        // Scalar Double ALU & Comparisons
        case FO::AddDouble:
        case FO::SubDouble:
        case FO::MulDouble:
        case FO::DivDouble:
        case FO::AddDoubleTrusted:
        case FO::SubDoubleTrusted:
        case FO::MulDoubleTrusted:
        case FO::DivDoubleTrusted: {
            const bool trusted =
                fi.flatOpcode >= FO::AddDoubleTrusted && fi.flatOpcode <= FO::DivDoubleTrusted;
            if (!trusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, unexecAfter);
            }
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movsdLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            std::uint8_t sseOp = 0x58u;
            if (fi.flatOpcode == FO::SubDouble || fi.flatOpcode == FO::SubDoubleTrusted) sseOp = 0x5Cu;
            else if (fi.flatOpcode == FO::MulDouble || fi.flatOpcode == FO::MulDoubleTrusted) sseOp = 0x59u;
            else if (fi.flatOpcode == FO::DivDouble || fi.flatOpcode == FO::DivDoubleTrusted) sseOp = 0x5Eu;
            as.binarySd(sseOp, 0, 1);
            as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;
        }

        case FO::LessDouble:
        case FO::LessOrEqualDouble:
        case FO::GreaterDouble:
        case FO::GreaterOrEqualDouble: {
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movsdLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::LessDouble) {
                as.ucomisd(1, 0);
                as.setccAl(X64Cond::A);
            } else if (fi.flatOpcode == FO::LessOrEqualDouble) {
                as.ucomisd(1, 0);
                as.setccAl(X64Cond::AE);
            } else if (fi.flatOpcode == FO::GreaterDouble) {
                as.ucomisd(0, 1);
                as.setccAl(X64Cond::A);
            } else {
                as.ucomisd(0, 1);
                as.setccAl(X64Cond::AE);
            }
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;
        }

        case FO::NegateDouble:
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movImm64(X64Reg::RCX, 0x8000000000000000ULL);
            as.emitRex(true, static_cast<std::uint8_t>(X64Reg::RCX), 0, static_cast<std::uint8_t>(X64Reg::RAX));
            as.emit8(0x31u); // xor rax, rcx
            as.emitModRMReg(static_cast<std::uint8_t>(X64Reg::RCX), static_cast<std::uint8_t>(X64Reg::RAX));
            emitWriteTaggedResult(fi.dstSlot, kTagDouble);
            break;

        case FO::SuperBinaryLocalDoubleConstStore:
        case FO::SuperBinaryLocalDoubleConstStoreElided:
        case FO::SuperBinaryLocalDoubleLocalStore:
        case FO::SuperBinaryLocalDoubleLocalStoreElided: {
            const bool isConst =
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStore ||
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStoreElided;
            const bool isElided =
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalDoubleLocalStoreElided;
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, 1);
            as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            if (isConst) {
                as.movImm64(X64Reg::RAX, static_cast<std::uint64_t>(fi.integerImmediate));
                as.movqToXmm(1, X64Reg::RAX);
            } else {
                emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, 1);
                as.movsdLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            }
            if (!isElided) {
                as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
                as.movsdStore(X64Reg::RBX, slotOff(fi.aux0Slot), 1);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux0Slot) + 15, kTagDouble);
            }
            std::uint8_t sseOp = 0x58u;
            if (fi.binaryOpcode1 == QO::SubtractDouble) sseOp = 0x5Cu;
            else if (fi.binaryOpcode1 == QO::MultiplyDouble) sseOp = 0x59u;
            else if (fi.binaryOpcode1 == QO::DivideDouble) sseOp = 0x5Eu;
            as.binarySd(sseOp, 0, 1);
            if (!isElided) {
                as.movsdStore(X64Reg::RBX, slotOff(fi.aux1Slot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux1Slot) + 15, kTagDouble);
            }
            as.movsdStore(X64Reg::RBX, slotOff(fi.storeSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.storeSlot) + 15, kTagDouble);
            break;
        }

        // Scalar Float ALU & Superinstructions
        case FO::AddFloat:
        case FO::SubFloat:
        case FO::MulFloat:
        case FO::DivFloat:
        case FO::AddFloatTrusted:
        case FO::SubFloatTrusted:
        case FO::MulFloatTrusted: {
            const bool trusted =
                fi.flatOpcode >= FO::AddFloatTrusted && fi.flatOpcode <= FO::MulFloatTrusted;
            if (!trusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, unexecAfter);
            }
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movssLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            std::uint8_t sseOp = 0x58u;
            if (fi.flatOpcode == FO::SubFloat || fi.flatOpcode == FO::SubFloatTrusted) sseOp = 0x5Cu;
            else if (fi.flatOpcode == FO::MulFloat || fi.flatOpcode == FO::MulFloatTrusted) sseOp = 0x59u;
            else if (fi.flatOpcode == FO::DivFloat) sseOp = 0x5Eu;
            as.binarySs(sseOp, 0, 1);
            as.movssStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::LessFloat:
        case FO::LessOrEqualFloat:
        case FO::GreaterFloat:
        case FO::GreaterOrEqualFloat:
        case FO::EqualFloat:
        case FO::NotEqualFloat: {
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movssLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::LessFloat) {
                as.ucomiss(1, 0);
                as.setccAl(X64Cond::A);
            } else if (fi.flatOpcode == FO::LessOrEqualFloat) {
                as.ucomiss(1, 0);
                as.setccAl(X64Cond::AE);
            } else if (fi.flatOpcode == FO::GreaterFloat) {
                as.ucomiss(0, 1);
                as.setccAl(X64Cond::A);
            } else if (fi.flatOpcode == FO::GreaterOrEqualFloat) {
                as.ucomiss(0, 1);
                as.setccAl(X64Cond::AE);
            } else if (fi.flatOpcode == FO::EqualFloat) {
                as.ucomiss(0, 1);
                as.setccAl(X64Cond::E);
                as.setccCl(X64Cond::NP);
                as.emit8(0x20u); // and al, cl
                as.emit8(0xC8u);
            } else {
                as.ucomiss(0, 1);
                as.setccAl(X64Cond::NE);
                as.setccCl(X64Cond::P);
                as.emit8(0x08u); // or al, cl
                as.emit8(0xC8u);
            }
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;
        }

        case FO::NegateFloat:
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem32(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.emit8(0x35u); // xor eax, 0x80000000
            as.emit32(0x80000000u);
            as.storeMem32(X64Reg::RBX, slotOff(fi.dstSlot), X64Reg::RAX);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;

        case FO::SuperBinaryLocalFloatLocalStore:
        case FO::SuperBinaryLocalFloatLocalStoreElided:
        case FO::SuperBinaryLocalFloatReg:
        case FO::SuperBinaryLocalFloatRegElided:
        case FO::SuperBinaryLocalFloatRegStore:
        case FO::SuperBinaryLocalFloatRegStoreElided:
        case FO::SuperBinaryRegFloatStoreElided: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, 0);
            emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, 0);
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movssLoad(1, X64Reg::RBX, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
                as.movssStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
                if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore) {
                    as.movssStore(X64Reg::RBX, slotOff(fi.aux0Slot), 1);
                    as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux0Slot) + 15, kTagFloat);
                }
            }
            std::uint8_t sseOp = 0x58u;
            if (fi.binaryOpcode1 == QO::SubtractFloat) sseOp = 0x5Cu;
            else if (fi.binaryOpcode1 == QO::MultiplyFloat) sseOp = 0x59u;
            else if (fi.binaryOpcode1 == QO::DivideFloat) sseOp = 0x5Eu;
            as.binarySs(sseOp, 0, 1);
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegElided ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
                as.movssStore(X64Reg::RBX, slotOff(fi.aux1Slot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.aux1Slot) + 15, kTagFloat);
            }
            if (fi.flatOpcode != FO::SuperBinaryLocalFloatReg &&
                fi.flatOpcode != FO::SuperBinaryLocalFloatRegElided) {
                as.movssStore(X64Reg::RBX, slotOff(fi.storeSlot), 0);
                as.storeMem8Imm(X64Reg::RBX, slotOff(fi.storeSlot) + 15, kTagFloat);
            }
            break;
        }

        // Fast Scalar Conversions
        case FO::ConvertIntToLongFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            emitWriteTaggedResult(fi.dstSlot, kTagLong);
            break;

        case FO::ConvertIntToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtsi2sd64(0, X64Reg::RAX);
            as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertLongToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagLong, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtsi2sd64(0, X64Reg::RAX);
            as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertIntToFloatFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtsi2ss64(0, X64Reg::RAX);
            as.movssStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;

        case FO::ConvertFloatToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtss2sd(0, 0);
            as.movsdStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertDoubleToFloatFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.checked1() && fi.span > 1u);
            as.movsdLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            if (fi.checked1()) {
                constexpr double kMaxF = static_cast<double>(std::numeric_limits<float>::max());
                constexpr double kMinF = -kMaxF;
                std::uint64_t maxBits = 0, minBits = 0;
                std::memcpy(&maxBits, &kMaxF, 8);
                std::memcpy(&minBits, &kMinF, 8);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
                as.movImm64(X64Reg::RAX, minBits);
                as.movqToXmm(1, X64Reg::RAX);
                as.ucomisd(0, 1);
                as.jcc(X64Cond::P, ovfStub);
                as.jcc(X64Cond::B, ovfStub);
                as.movImm64(X64Reg::RAX, maxBits);
                as.movqToXmm(1, X64Reg::RAX);
                as.ucomisd(1, 0);
                as.jcc(X64Cond::B, ovfStub);
            }
            as.cvtsd2ss(0, 0);
            as.movssStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(X64Reg::RBX, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::ConvertFloatToIntFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.span > 1u);
            as.movssLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.cvtss2sd(0, 0);
            constexpr double kMinI = static_cast<double>(std::numeric_limits<std::int32_t>::min());
            constexpr double kMaxI = static_cast<double>(std::numeric_limits<std::int32_t>::max());
            std::uint64_t minBits = 0, maxBits = 0;
            std::memcpy(&minBits, &kMinI, 8);
            std::memcpy(&maxBits, &kMaxI, 8);
            const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
            as.movImm64(X64Reg::RAX, minBits);
            as.movqToXmm(1, X64Reg::RAX);
            as.ucomisd(0, 1);
            as.jcc(X64Cond::P, ovfStub);
            as.jcc(X64Cond::B, ovfStub);
            as.movImm64(X64Reg::RAX, maxBits);
            as.movqToXmm(1, X64Reg::RAX);
            as.ucomisd(1, 0);
            as.jcc(X64Cond::B, ovfStub);
            // cvttsd2si rax, xmm0
            as.emit8(0xF2u);
            as.emitRex(true, 0, 0, 0);
            as.emit8(0x0Fu);
            as.emit8(0x2Cu);
            as.emitModRMReg(0, 0);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;
        }

        case FO::ConvertLongToIntFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagLong, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.checked1() && fi.span > 1u);
            as.loadMem64(X64Reg::RAX, X64Reg::RBX, slotOff(fi.src0Slot));
            as.movsxd64From32(X64Reg::RCX, X64Reg::RAX);
            if (fi.checked1()) {
                as.cmpReg64(X64Reg::RAX, X64Reg::RCX);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
                as.jcc(X64Cond::NE, ovfStub);
            }
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;
        }

        // Unfolded Leaf Function Inlining Opcodes
        case FO::InlineCallEnter: {
            emitHelperCallInline(&jitInlineCallEnterHelper, i32Idx);
            const std::size_t fastTarget = idx + 1u + fi.inlineArgCount;
            if (fi.inlineArgCount > 0u && fastTarget < nInst) {
                as.loadMem32(
                    X64Reg::RAX,
                    X64Reg::R12,
                    static_cast<std::int32_t>(offsetof(JitAbiContext, nextFlatOffsetOut)));
                as.testReg32(X64Reg::RAX, X64Reg::RAX);
                as.jcc(X64Cond::NE, instLabels[fastTarget]);
            }
            break;
        }

        case FO::InlineReturnVoid:
        case FO::InlineReturnVoidJump: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.integerImmediate != 0) {
                const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                const auto clearBase = static_cast<std::uint16_t>(packed & 0xFFFFu);
                const auto clearCount = static_cast<std::uint32_t>(packed >> 16u);
                as.xorpsSelf(0);
                for (std::uint32_t k = 0; k < clearCount; ++k) {
                    as.movupsStore(X64Reg::RBX, slotOff(static_cast<std::uint16_t>(clearBase + k)), 0);
                }
            }
            if (fi.flatOpcode == FO::InlineReturnVoidJump) {
                as.jmp(instLabels[fi.trueTargetOffset]);
            }
            break;
        }

        case FO::InlineReturnScalar:
        case FO::InlineReturnScalarJump: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            }
            if (fi.integerImmediate != 0) {
                const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                const auto clearBase = static_cast<std::uint16_t>(packed & 0xFFFFu);
                const auto clearCount = static_cast<std::uint32_t>(packed >> 16u);
                as.xorpsSelf(1);
                for (std::uint32_t k = 0; k < clearCount; ++k) {
                    as.movupsStore(X64Reg::RBX, slotOff(static_cast<std::uint16_t>(clearBase + k)), 1);
                }
            }
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            }
            if (fi.flatOpcode == FO::InlineReturnScalarJump) {
                as.jmp(instLabels[fi.trueTargetOffset]);
            }
            break;
        }

        // Terminators
        case FO::TermJumpNoArgs:
            emitBudgetDeduct(i32Idx, fi.span, fi.span > 1u);
            as.addImm64(X64Reg::R14, static_cast<std::int32_t>(fi.span));
            if (fi.trueTargetOffset != idx + 1u) {
                as.jmp(instLabels[fi.trueTargetOffset]);
            }
            break;

        case FO::TermBranchBoolNoArgs:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.inc64(X64Reg::R14);
            as.cmpMem8Imm(X64Reg::RBX, slotOff(fi.src0Slot), 0u);
            emitCondBranchTargets(X64Cond::NE, i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;

        case FO::TermBranchGeneric: {
            const auto& cold = execFunc.flatCold[idx];
            if (cold.blockIndex >= execFunc.blocks.size()) return {};
            const auto& blk = execFunc.blocks[cold.blockIndex];
            const auto& term = blk.bytecode->terminator;

            const auto emitEdgeCopies = [&](const runtime::detail::ExecutionEdge& edge) -> bool {
                if (!edge.hasArguments) return true;
                if (!edge.sources || !edge.destinations ||
                    edge.sources->size() != edge.destinations->size() ||
                    edge.sources->size() > execFunc.edgeScratchCount) {
                    return false;
                }
                const auto& srcs = *edge.sources;
                const auto& dsts = *edge.destinations;
                if (srcs.size() == 1u) {
                    const auto sSlot = static_cast<std::uint16_t>(execFunc.registerOffset + srcs[0]);
                    const auto dSlot = static_cast<std::uint16_t>(execFunc.registerOffset + dsts[0].target);
                    if (sSlot != dSlot) {
                        as.movupsLoad(0, X64Reg::RBX, slotOff(sSlot));
                        as.movupsStore(X64Reg::RBX, slotOff(dSlot), 0);
                    }
                    return true;
                }
                for (std::size_t k = 0; k < srcs.size(); ++k) {
                    const auto sSlot = static_cast<std::uint16_t>(execFunc.registerOffset + srcs[k]);
                    const auto scSlot = static_cast<std::uint16_t>(execFunc.scratchOffset + k);
                    as.movupsLoad(0, X64Reg::RBX, slotOff(sSlot));
                    as.movupsStore(X64Reg::RBX, slotOff(scSlot), 0);
                }
                for (std::size_t k = 0; k < srcs.size(); ++k) {
                    const auto scSlot = static_cast<std::uint16_t>(execFunc.scratchOffset + k);
                    const auto dSlot = static_cast<std::uint16_t>(execFunc.registerOffset + dsts[k].target);
                    as.movupsLoad(0, X64Reg::RBX, slotOff(scSlot));
                    as.movupsStore(X64Reg::RBX, slotOff(dSlot), 0);
                }
                return true;
            };

            emitBudgetDeduct(i32Idx, fi.span, false);
            if (term.kind == bytecode::TerminatorKind::Jump) {
                as.inc64(X64Reg::R14);
                if (!emitEdgeCopies(blk.branch)) return {};
                if (fi.trueTargetOffset != idx + 1u) {
                    as.jmp(instLabels[fi.trueTargetOffset]);
                }
            } else if (term.kind == bytecode::TerminatorKind::Branch) {
                const auto condSlot = static_cast<std::uint16_t>(execFunc.registerOffset + term.condition);
                if (condSlot >= execFunc.totalFrameSlots ||
                    fi.falseTargetOffset >= nInst) {
                    return {};
                }
                emitCheckTagOrTrap(condSlot, kTagBool, i32Idx, 0);
                as.inc64(X64Reg::R14);
                const std::uint32_t falsePathLbl = as.createLabel();
                as.cmpMem8Imm(X64Reg::RBX, slotOff(condSlot), 0u);
                as.jcc(X64Cond::E, falsePathLbl);
                if (!emitEdgeCopies(blk.branch)) return {};
                as.jmp(instLabels[fi.trueTargetOffset]);
                as.bind(falsePathLbl);
                if (!emitEdgeCopies(blk.falseBranch)) return {};
                if (fi.falseTargetOffset != idx + 1u) {
                    as.jmp(instLabels[fi.falseTargetOffset]);
                }
            } else {
                return {};
            }
            break;
        }

        case FO::TermReturnVoid:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(X64Reg::RAX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.xorpsSelf(0);
            as.movupsStore(X64Reg::RAX, 0, 0);
            as.jmp(epilogueOk);
            break;

        case FO::TermReturnScalarFast:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
            as.loadMem64(X64Reg::RAX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.movupsStore(X64Reg::RAX, 0, 0);
            as.jmp(epilogueOk);
            break;

        case FO::TermReturnGeneric: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            const auto& cold = execFunc.flatCold[idx];
            const auto retReg = execFunc.blocks[cold.blockIndex].bytecode->terminator.value;
            const auto retSlot = static_cast<std::uint16_t>(execFunc.registerOffset + retReg);
            as.movupsLoad(0, X64Reg::RBX, slotOff(retSlot));
            as.loadMem64(X64Reg::RAX, X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.movupsStore(X64Reg::RAX, 0, 0);
            as.jmp(epilogueOk);
            break;
        }

        default:
            return {};
        }
    }

    // ========================================================================
    // Out-of-line Cold Stubs (keeps hot loop I-cache footprint maximally dense)
    // ========================================================================
    for (const auto& stub : coldStubs) {
        as.bind(stub.stubLabel);
        switch (stub.kind) {
        case ColdTrapStub::Kind::BudgetSimple:
            as.storeMem32Imm(
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.jmp(epilogueBudget);
            break;

        case ColdTrapStub::Kind::BudgetPartialSuper:
            as.addImm64(X64Reg::R13, stub.restoreBudget);
            emitHelperCallInline(&jitPartialSuperBudgetHelper, stub.flatIdx);
            as.jmp(epilogueBudget);
            break;

        case ColdTrapStub::Kind::IntegerOverflow:
            if (stub.restoreBudget != 0) as.addImm64(X64Reg::R13, stub.restoreBudget);
            as.storeMem32Imm(
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.jmp(epilogueOverflow);
            break;

        case ColdTrapStub::Kind::DivisionByZero:
            if (stub.restoreBudget != 0) as.addImm64(X64Reg::R13, stub.restoreBudget);
            as.storeMem32Imm(
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.jmp(epilogueDivZero);
            break;

        case ColdTrapStub::Kind::TypeMismatch:
            if (stub.restoreBudget != 0) as.addImm64(X64Reg::R13, stub.restoreBudget);
            as.storeMem32Imm(
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.jmp(epilogueTypeErr);
            break;

        case ColdTrapStub::Kind::ArrayLengthSlow:
            emitHelperCallInline(&jitArrayLengthHelper, stub.flatIdx);
            as.jmp(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayLengthBranchSlow:
            emitHelperCallInline(&jitArrayLengthHelper, stub.flatIdx);
            as.loadMem32(
                X64Reg::RAX,
                X64Reg::R12,
                static_cast<std::int32_t>(offsetof(JitAbiContext, branchCondOut)));
            as.testReg32(X64Reg::RAX, X64Reg::RAX);
            as.jcc(X64Cond::NE, stub.resumeLabel);
            as.jmp(stub.altResumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayLoadSlow:
            emitHelperCallInline(&jitLoadElementHelper, stub.flatIdx);
            as.jmp(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayStoreSlow:
            emitHelperCallInline(&jitStoreElementHelper, stub.flatIdx);
            as.jmp(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::FlatStructSlow:
            emitHelperCallInline(&jitFlatStructAndStackRefHelper, stub.flatIdx);
            as.jmp(stub.resumeLabel);
            break;
        }
    }

    // Shared Exit Epilogues
    as.bind(epilogueOk);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.xorReg32(X64Reg::RAX, X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueBudget);
    as.xorReg32(X64Reg::R13, X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.movImm32(X64Reg::RAX, static_cast<std::uint32_t>(runtime::ErrorCode::InstructionBudgetExceeded));
    as.storeMem32(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueOverflow);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.movImm32(X64Reg::RAX, static_cast<std::uint32_t>(runtime::ErrorCode::IntegerOverflow));
    as.storeMem32(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueDivZero);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.movImm32(X64Reg::RAX, static_cast<std::uint32_t>(runtime::ErrorCode::DivisionByZero));
    as.storeMem32(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueTypeErr);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), X64Reg::R13);
    as.storeMem64(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), X64Reg::R14);
    as.movImm32(X64Reg::RAX, static_cast<std::uint32_t>(runtime::ErrorCode::TypeMismatch));
    as.storeMem32(X64Reg::R12, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), X64Reg::RAX);
    as.jmp(sharedPopRet);

    as.bind(epilogueHelperErr);
    // Helper already synced remainingBudget/branchesTaken and populated session.error; EAX has ErrorCode

    as.bind(sharedPopRet);
    as.addImm64(X64Reg::RSP, 40);
    as.pop(X64Reg::R15);
    as.pop(X64Reg::R14);
    as.pop(X64Reg::R13);
    as.pop(X64Reg::R12);
    as.pop(X64Reg::RDI);
    as.pop(X64Reg::RSI);
    as.pop(X64Reg::RBX);
    as.pop(X64Reg::RBP);
    as.ret();

    if (!as.allLabelsBound()) return {};
    return as.bytes();
}

// ============================================================================
// P2-A Tier 2B: Native ARM64 (AArch64) Stencil Machine Code Emitter
// Covers macOS Apple Silicon (Darwin ARM64), iOS, Android/Linux AAPCS64 & Win-ARM64
// Strictly avoids platform-reserved register X18 across all OS targets!
// ============================================================================

enum class A64Reg : std::uint8_t {
    X0 = 0,
    X1 = 1,
    X2 = 2,
    X3 = 3,
    X4 = 4,
    X5 = 5,
    X6 = 6,
    X7 = 7,
    X8 = 8,
    X9 = 9,   // Primary address-offset scratch register
    X10 = 10, // Primary immediate/tag scratch register
    X11 = 11, // Auxiliary loop/slice scratch register
    X12 = 12,
    X13 = 13,
    X14 = 14,
    X15 = 15,
    X16 = 16, // IP0: C++ helper call target register (`blr x16`)
    X17 = 17, // IP1
    // NOTE: X18 is INTENTIONALLY OMITTED (platform-reserved on Apple Darwin ARM64 & Windows ARM64)
    X19 = 19, // Callee-saved: pinned `JitAbiContext* ctx`
    X20 = 20, // Callee-saved: pinned `runtime::Value* frameBase`
    X21 = 21, // Callee-saved: pinned `uint64_t remainingBudget`
    X22 = 22, // Callee-saved: pinned `uint64_t branchesTaken`
    X23 = 23, // Callee-saved
    X24 = 24, // Callee-saved
    X29 = 29, // FP
    X30 = 30, // LR
    XZR = 31, // Zero register (or SP in stack/immediate base contexts)
};

enum class A64Cond : std::uint8_t {
    EQ = 0x0, // Z == 1
    NE = 0x1, // Z == 0
    HS = 0x2, // C == 1 (unsigned >=, CS)
    LO = 0x3, // C == 0 (unsigned <,  CC / borrow on SUBS)
    MI = 0x4, // N == 1 (FCMP ordered <)
    PL = 0x5, // N == 0
    VS = 0x6, // V == 1 (signed overflow / FCMP unordered NaN)
    VC = 0x7, // V == 0
    HI = 0x8, // C == 1 && Z == 0 (unsigned >)
    LS = 0x9, // !(C == 1 && Z == 0) (unsigned <= / FCMP ordered <=)
    GE = 0xA, // N == V (signed >= / FCMP ordered >=)
    LT = 0xB, // N != V (signed <)
    GT = 0xC, // Z == 0 && N == V (signed > / FCMP ordered >)
    LE = 0xD, // Z == 1 || N != V (signed <=)
    AL = 0xE,
};

[[nodiscard]] constexpr A64Cond invertA64Cond(A64Cond cc) noexcept {
    return static_cast<A64Cond>(static_cast<std::uint8_t>(cc) ^ 1u);
}

[[nodiscard]] constexpr A64Cond quickCmpToA64Cond(runtime::detail::QuickOpcode op) noexcept {
    using QO = runtime::detail::QuickOpcode;
    switch (op) {
    case QO::LessInt: return A64Cond::LT;
    case QO::LessOrEqualInt: return A64Cond::LE;
    case QO::GreaterInt: return A64Cond::GT;
    case QO::GreaterOrEqualInt: return A64Cond::GE;
    case QO::EqualInt: return A64Cond::EQ;
    case QO::NotEqualInt: return A64Cond::NE;
    default: return A64Cond::EQ;
    }
}

class A64Emitter {
public:
    enum class PatchKind : std::uint8_t {
        BImm26,
        BCondImm19,
    };

    struct PatchSite {
        std::size_t bytePos = 0;
        PatchKind kind = PatchKind::BImm26;
    };

    struct Label {
        std::size_t boundOffset = std::numeric_limits<std::size_t>::max();
        std::vector<PatchSite> patches;
        [[nodiscard]] bool bound() const noexcept {
            return boundOffset != std::numeric_limits<std::size_t>::max();
        }
    };

    std::uint32_t createLabel() {
        const auto id = static_cast<std::uint32_t>(labels_.size());
        labels_.emplace_back();
        return id;
    }

    void bind(std::uint32_t labelId) {
        auto& lbl = labels_[labelId];
        lbl.boundOffset = buf_.size();
        for (const auto& site : lbl.patches) {
            patchSite(site, lbl.boundOffset);
        }
        lbl.patches.clear();
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] bool ok() const noexcept {
        if (overflowed_ || (buf_.size() % 4u) != 0u) return false;
        for (const auto& lbl : labels_) {
            if (!lbl.patches.empty() && !lbl.bound()) return false;
        }
        return true;
    }

    void emitInst(std::uint32_t word) {
        std::uint8_t tmp[4];
        std::memcpy(tmp, &word, 4);
        buf_.insert(buf_.end(), tmp, tmp + 4);
    }

    // Stack frame prologue/epilogue helpers
    void subSp(std::uint16_t imm12) {
        emitInst(0xD1000000u | ((static_cast<std::uint32_t>(imm12) & 0xFFFu) << 10) | (31u << 5) | 31u);
    }
    void addSp(std::uint16_t imm12) {
        emitInst(0x91000000u | ((static_cast<std::uint32_t>(imm12) & 0xFFFu) << 10) | (31u << 5) | 31u);
    }
    void movFpFromSp() {
        emitInst(0x910003FDu); // add x29, sp, #0
    }
    void stp64Sp(A64Reg rt1, A64Reg rt2, std::int32_t byteOff) {
        const auto imm7 = static_cast<std::uint32_t>((byteOff / 8) & 0x7F);
        emitInst(0xA9000000u | (imm7 << 15) | (regCode(rt2) << 10) | (31u << 5) | regCode(rt1));
    }
    void ldp64Sp(A64Reg rt1, A64Reg rt2, std::int32_t byteOff) {
        const auto imm7 = static_cast<std::uint32_t>((byteOff / 8) & 0x7F);
        emitInst(0xA9400000u | (imm7 << 15) | (regCode(rt2) << 10) | (31u << 5) | regCode(rt1));
    }
    void ret() {
        emitInst(0xD65F03C0u);
    }
    void blr(A64Reg target) {
        emitInst(0xD63F0000u | (regCode(target) << 5));
    }

    // Register moves & wide immediates
    void movReg64(A64Reg dst, A64Reg src) {
        if (dst == src) return;
        emitInst(0xAA0003E0u | (regCode(src) << 16) | regCode(dst));
    }
    void movReg32(A64Reg dst, A64Reg src) {
        if (dst == src) return;
        emitInst(0x2A0003E0u | (regCode(src) << 16) | regCode(dst));
    }
    void movz32(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0x52800000u | (static_cast<std::uint32_t>(hw & 1u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movn32(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0x12800000u | (static_cast<std::uint32_t>(hw & 1u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movk32(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0x72800000u | (static_cast<std::uint32_t>(hw & 1u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movz64(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0xD2800000u | (static_cast<std::uint32_t>(hw & 3u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movn64(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0x92800000u | (static_cast<std::uint32_t>(hw & 3u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }
    void movk64(A64Reg dst, std::uint16_t imm16, std::uint8_t hw = 0) {
        emitInst(0xF2800000u | (static_cast<std::uint32_t>(hw & 3u) << 21) |
                 (static_cast<std::uint32_t>(imm16) << 5) | regCode(dst));
    }

    void movImm32(A64Reg dst, std::uint32_t val) {
        if ((val >> 16u) == 0u) {
            movz32(dst, static_cast<std::uint16_t>(val), 0);
            return;
        }
        const std::uint32_t inv = ~val;
        if ((inv >> 16u) == 0u) {
            movn32(dst, static_cast<std::uint16_t>(inv), 0);
            return;
        }
        movz32(dst, static_cast<std::uint16_t>(val & 0xFFFFu), 0);
        movk32(dst, static_cast<std::uint16_t>(val >> 16u), 1);
    }

    void movImm64(A64Reg dst, std::uint64_t val) {
        if (val == 0u) {
            movz32(dst, 0u, 0);
            return;
        }
        const std::uint64_t inv = ~val;
        if ((inv >> 16u) == 0u) {
            movn64(dst, static_cast<std::uint16_t>(inv & 0xFFFFu), 0);
            return;
        }
        bool first = true;
        for (std::uint8_t hw = 0; hw < 4u; ++hw) {
            const auto chunk = static_cast<std::uint16_t>((val >> (hw * 16u)) & 0xFFFFu);
            if (chunk == 0u) continue;
            if (first) {
                movz64(dst, chunk, hw);
                first = false;
            } else {
                movk64(dst, chunk, hw);
            }
        }
    }

    // Immediate & register 64-bit / 32-bit arithmetic
    void addImm64(A64Reg dst, A64Reg src, std::int64_t imm) {
        if (imm == 0) {
            movReg64(dst, src);
            return;
        }
        if (imm < 0) {
            subImm64(dst, src, -imm);
            return;
        }
        if (imm <= 4095) {
            emitInst(0x91000000u | (static_cast<std::uint32_t>(imm) << 10) | (regCode(src) << 5) | regCode(dst));
            return;
        }
        if (imm <= 0xFFFFFFLL) {
            const auto hi12 = static_cast<std::uint32_t>((imm >> 12) & 0xFFF);
            const auto lo12 = static_cast<std::uint32_t>(imm & 0xFFF);
            emitInst(0x91400000u | (hi12 << 10) | (regCode(src) << 5) | regCode(dst));
            if (lo12 != 0u) {
                emitInst(0x91000000u | (lo12 << 10) | (regCode(dst) << 5) | regCode(dst));
            }
            return;
        }
        const A64Reg scratch = (dst == A64Reg::X10 || src == A64Reg::X10) ? A64Reg::X11 : A64Reg::X10;
        movImm64(scratch, static_cast<std::uint64_t>(imm));
        addReg64(dst, src, scratch, 0);
    }

    void subImm64(A64Reg dst, A64Reg src, std::int64_t imm) {
        if (imm == 0) {
            movReg64(dst, src);
            return;
        }
        if (imm < 0) {
            addImm64(dst, src, -imm);
            return;
        }
        if (imm <= 4095) {
            emitInst(0xD1000000u | (static_cast<std::uint32_t>(imm) << 10) | (regCode(src) << 5) | regCode(dst));
            return;
        }
        const A64Reg scratch = (dst == A64Reg::X10 || src == A64Reg::X10) ? A64Reg::X11 : A64Reg::X10;
        movImm64(scratch, static_cast<std::uint64_t>(imm));
        subReg64(dst, src, scratch);
    }

    // `subs xDst, xSrc, #uimm12` -> sets C=0 (A64Cond::LO) on unsigned borrow!
    void subsImm64(A64Reg dst, A64Reg src, std::uint16_t imm12) {
        emitInst(0xF1000000u | ((static_cast<std::uint32_t>(imm12) & 0xFFFu) << 10) |
                 (regCode(src) << 5) | regCode(dst));
    }

    void subsImm32(A64Reg dst, A64Reg src, std::uint16_t imm12) {
        emitInst(0x71000000u | ((static_cast<std::uint32_t>(imm12) & 0xFFFu) << 10) |
                 (regCode(src) << 5) | regCode(dst));
    }

    void addReg64(A64Reg dst, A64Reg lhs, A64Reg rhs, std::uint8_t lslShift = 0) {
        emitInst(0x8B000000u | (regCode(rhs) << 16) |
                 ((static_cast<std::uint32_t>(lslShift) & 63u) << 10) |
                 (regCode(lhs) << 5) | regCode(dst));
    }

    void subReg64(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0xCB000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }

    void addReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x0B000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void addsReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x2B000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void subReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x4B000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void subsReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x6B000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }

    void mulReg64(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x9B007C00u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void smullReg(A64Reg dst64, A64Reg lhs32, A64Reg rhs32) {
        emitInst(0x9B207C00u | (regCode(rhs32) << 16) | (regCode(lhs32) << 5) | regCode(dst64));
    }
    void sdivReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x1AC00C00u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void msubReg32(A64Reg dst, A64Reg mulLhs, A64Reg mulRhs, A64Reg minuend) {
        emitInst(0x1B008000u | (regCode(mulRhs) << 16) | (regCode(minuend) << 10) |
                 (regCode(mulLhs) << 5) | regCode(dst));
    }

    void sxtw(A64Reg dst64, A64Reg src32) {
        emitInst(0x93407C00u | (regCode(src32) << 5) | regCode(dst64));
    }
    void lslImm64(A64Reg dst, A64Reg src, std::uint8_t shift) {
        if (shift == 0) {
            movReg64(dst, src);
            return;
        }
        const std::uint32_t immr = (64u - (shift & 63u)) & 63u;
        const std::uint32_t imms = 63u - (shift & 63u);
        emitInst(0xD3400000u | (immr << 16) | (imms << 10) | (regCode(src) << 5) | regCode(dst));
    }
    void lsrImm32(A64Reg dst, A64Reg src, std::uint8_t shift) {
        if (shift == 0) {
            movReg32(dst, src);
            return;
        }
        emitInst(0x53007C00u | (static_cast<std::uint32_t>(shift & 31u) << 16) |
                 (regCode(src) << 5) | regCode(dst));
    }
    void andLow24Bits32(A64Reg dst, A64Reg src) {
        // ubfx wd, ws, #0, #24 (ubfm wd, ws, #0, #23)
        emitInst(0x53005C00u | (regCode(src) << 5) | regCode(dst));
    }

    void orrReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x2A000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void eorReg32(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0x4A000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }
    void eorReg64(A64Reg dst, A64Reg lhs, A64Reg rhs) {
        emitInst(0xCA000000u | (regCode(rhs) << 16) | (regCode(lhs) << 5) | regCode(dst));
    }

    void cmpReg32(A64Reg lhs, A64Reg rhs) {
        subsReg32(A64Reg::XZR, lhs, rhs);
    }
    void cmpReg64(A64Reg lhs, A64Reg rhs) {
        emitInst(0xEB00001Fu | (regCode(rhs) << 16) | (regCode(lhs) << 5));
    }
    void cmpImm32(A64Reg lhs, std::int32_t imm) {
        if (imm >= 0 && imm <= 4095) {
            subsImm32(A64Reg::XZR, lhs, static_cast<std::uint16_t>(imm));
        } else if (imm < 0 && imm >= -4095) {
            // cmn wn, #-imm (adds wzr, wn, #-imm)
            emitInst(0x3100001Fu | (static_cast<std::uint32_t>(-imm) << 10) | (regCode(lhs) << 5));
        } else {
            const A64Reg sc = (lhs == A64Reg::X10) ? A64Reg::X11 : A64Reg::X10;
            movImm32(sc, static_cast<std::uint32_t>(imm));
            cmpReg32(lhs, sc);
        }
    }
    void cmpImm64(A64Reg lhs, std::int64_t imm) {
        if (imm >= 0 && imm <= 4095) {
            subsImm64(A64Reg::XZR, lhs, static_cast<std::uint16_t>(imm));
        } else if (imm < 0 && imm >= -4095) {
            emitInst(0xB100001Fu | (static_cast<std::uint32_t>(-imm) << 10) | (regCode(lhs) << 5));
        } else {
            const A64Reg sc = (lhs == A64Reg::X10) ? A64Reg::X11 : A64Reg::X10;
            movImm64(sc, static_cast<std::uint64_t>(imm));
            cmpReg64(lhs, sc);
        }
    }

    void cset32(A64Reg dst, A64Cond cc) {
        emitInst(0x1A9F07E0u | (static_cast<std::uint32_t>(invertA64Cond(cc)) << 12) | regCode(dst));
    }

    // Memory Loads & Stores (automatic X9 base materialization when offset exceeds uimm12*scale)
    void loadMem8ZeroExt(A64Reg dst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 1);
        emitInst(0x39400000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst));
    }
    void storeMem8Reg(A64Reg base, std::int32_t disp, A64Reg src) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 1);
        emitInst(0x39000000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(src));
    }
    void storeMem8Imm(A64Reg base, std::int32_t disp, std::uint8_t imm) {
        if (imm == 0u) {
            storeMem8Reg(base, disp, A64Reg::XZR);
        } else {
            movz32(A64Reg::X10, imm, 0);
            storeMem8Reg(base, disp, A64Reg::X10);
        }
    }
    void loadMem16ZeroExt(A64Reg dst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 2);
        emitInst(0x79400000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst));
    }
    void loadMem32(A64Reg dst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xB9400000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst));
    }
    void loadMem32SignExt64(A64Reg dst64, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xB9800000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst64));
    }
    void storeMem32(A64Reg base, std::int32_t disp, A64Reg src) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xB9000000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(src));
    }
    void storeMem32Imm(A64Reg base, std::int32_t disp, std::uint32_t imm) {
        if (imm == 0u) {
            storeMem32(base, disp, A64Reg::XZR);
        } else {
            movImm32(A64Reg::X10, imm);
            storeMem32(base, disp, A64Reg::X10);
        }
    }
    void loadMem64(A64Reg dst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 8);
        emitInst(0xF9400000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(dst));
    }
    void storeMem64(A64Reg base, std::int32_t disp, A64Reg src) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 8);
        emitInst(0xF9000000u | (uimm12 << 10) | (regCode(b) << 5) | regCode(src));
    }
    void storeMem64Imm(A64Reg base, std::int32_t disp, std::uint64_t imm) {
        if (imm == 0u) {
            storeMem64(base, disp, A64Reg::XZR);
        } else {
            movImm64(A64Reg::X10, imm);
            storeMem64(base, disp, A64Reg::X10);
        }
    }

    // NEON 128-bit & FP scalar loads/stores/ops (V0..V7 caller-saved across all ARM64 ABIs)
    void ldrQ(std::uint8_t vDst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 16);
        emitInst(0x3DC00000u | (uimm12 << 10) | (regCode(b) << 5) | (vDst & 31u));
    }
    void strQ(A64Reg base, std::int32_t disp, std::uint8_t vSrc) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 16);
        emitInst(0x3D800000u | (uimm12 << 10) | (regCode(b) << 5) | (vSrc & 31u));
    }
    void veorZero128(std::uint8_t vReg) {
        const std::uint32_t v = vReg & 31u;
        emitInst(0x6E201C00u | (v << 16) | (v << 5) | v);
    }

    void ldrD(std::uint8_t dDst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 8);
        emitInst(0xFD400000u | (uimm12 << 10) | (regCode(b) << 5) | (dDst & 31u));
    }
    void strD(A64Reg base, std::int32_t disp, std::uint8_t dSrc) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 8);
        emitInst(0xFD000000u | (uimm12 << 10) | (regCode(b) << 5) | (dSrc & 31u));
    }
    void ldrS(std::uint8_t sDst, A64Reg base, std::int32_t disp) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xBD400000u | (uimm12 << 10) | (regCode(b) << 5) | (sDst & 31u));
    }
    void strS(A64Reg base, std::int32_t disp, std::uint8_t sSrc) {
        auto [b, uimm12] = resolveScaledAddr(base, disp, 4);
        emitInst(0xBD000000u | (uimm12 << 10) | (regCode(b) << 5) | (sSrc & 31u));
    }

    void faddD(std::uint8_t dDst, std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E602800u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5) | dDst);
    }
    void fsubD(std::uint8_t dDst, std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E603800u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5) | dDst);
    }
    void fmulD(std::uint8_t dDst, std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E600800u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5) | dDst);
    }
    void fdivD(std::uint8_t dDst, std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E601800u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5) | dDst);
    }
    void fcmpD(std::uint8_t dLhs, std::uint8_t dRhs) {
        emitInst(0x1E602000u | (static_cast<std::uint32_t>(dRhs) << 16) | (static_cast<std::uint32_t>(dLhs) << 5));
    }

    void faddS(std::uint8_t sDst, std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E202800u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5) | sDst);
    }
    void fsubS(std::uint8_t sDst, std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E203800u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5) | sDst);
    }
    void fmulS(std::uint8_t sDst, std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E200800u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5) | sDst);
    }
    void fdivS(std::uint8_t sDst, std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E201800u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5) | sDst);
    }
    void fcmpS(std::uint8_t sLhs, std::uint8_t sRhs) {
        emitInst(0x1E202000u | (static_cast<std::uint32_t>(sRhs) << 16) | (static_cast<std::uint32_t>(sLhs) << 5));
    }

    void fmovDFromX(std::uint8_t dDst, A64Reg src64) {
        emitInst(0x9E670000u | (regCode(src64) << 5) | (dDst & 31u));
    }
    void scvtfDFromX(std::uint8_t dDst, A64Reg src64) {
        emitInst(0x9E620000u | (regCode(src64) << 5) | (dDst & 31u));
    }
    void scvtfSFromX(std::uint8_t sDst, A64Reg src64) {
        emitInst(0x9E220000u | (regCode(src64) << 5) | (sDst & 31u));
    }
    void fcvtDFromS(std::uint8_t dDst, std::uint8_t sSrc) {
        emitInst(0x1E22C000u | (static_cast<std::uint32_t>(sSrc) << 5) | dDst);
    }
    void fcvtSFromD(std::uint8_t sDst, std::uint8_t dSrc) {
        emitInst(0x1E624000u | (static_cast<std::uint32_t>(dSrc) << 5) | sDst);
    }
    void fcvtzsXFromD(A64Reg dst64, std::uint8_t dSrc) {
        emitInst(0x9E780000u | (static_cast<std::uint32_t>(dSrc) << 5) | regCode(dst64));
    }

    // Branches
    void b(std::uint32_t labelId) {
        const std::size_t pos = buf_.size();
        emitInst(0x14000000u);
        recordOrPatch(labelId, PatchSite{pos, PatchKind::BImm26});
    }
    void bcc(A64Cond cc, std::uint32_t labelId) {
        const std::size_t pos = buf_.size();
        emitInst(0x54000000u | static_cast<std::uint32_t>(cc));
        recordOrPatch(labelId, PatchSite{pos, PatchKind::BCondImm19});
    }

private:
    [[nodiscard]] static constexpr std::uint32_t regCode(A64Reg r) noexcept {
        return static_cast<std::uint32_t>(r) & 31u;
    }

    [[nodiscard]] std::pair<A64Reg, std::uint32_t> resolveScaledAddr(
        A64Reg base,
        std::int32_t disp,
        std::int32_t scale) {
        if (disp >= 0 && (disp % scale) == 0 && (disp / scale) <= 4095) {
            return {base, static_cast<std::uint32_t>(disp / scale)};
        }
        addImm64(A64Reg::X9, base, disp);
        return {A64Reg::X9, 0u};
    }

    void recordOrPatch(std::uint32_t labelId, PatchSite site) {
        auto& lbl = labels_[labelId];
        if (lbl.bound()) {
            patchSite(site, lbl.boundOffset);
        } else {
            lbl.patches.push_back(site);
        }
    }

    void patchSite(const PatchSite& site, std::size_t targetByteOffset) {
        const std::ptrdiff_t relBytes =
            static_cast<std::ptrdiff_t>(targetByteOffset) -
            static_cast<std::ptrdiff_t>(site.bytePos);
        if ((relBytes & 3) != 0) {
            overflowed_ = true;
            return;
        }
        const std::ptrdiff_t relWords = relBytes >> 2;
        std::uint32_t inst = 0;
        std::memcpy(&inst, buf_.data() + site.bytePos, 4);
        if (site.kind == PatchKind::BImm26) {
            if (relWords < -(1 << 25) || relWords >= (1 << 25)) {
                overflowed_ = true;
                return;
            }
            inst = (inst & 0xFC000000u) | (static_cast<std::uint32_t>(relWords) & 0x03FFFFFFu);
        } else {
            if (relWords < -(1 << 18) || relWords >= (1 << 18)) {
                overflowed_ = true;
                return;
            }
            inst = (inst & 0xFF00001Fu) | ((static_cast<std::uint32_t>(relWords) & 0x7FFFFu) << 5);
        }
        std::memcpy(buf_.data() + site.bytePos, &inst, 4);
    }

    std::vector<std::uint8_t> buf_;
    std::vector<Label> labels_;
    bool overflowed_ = false;
};

[[maybe_unused]] [[nodiscard]] std::vector<std::uint8_t> compileFunctionA64(
    const CompiledFunctionRecord& record) {
    const auto& execFunc = *record.execFunc;
    const std::size_t nInst = execFunc.flatStream.size();

    A64Emitter as;
    std::vector<std::uint32_t> instLabels(nInst);
    for (std::size_t i = 0; i < nInst; ++i) {
        instLabels[i] = as.createLabel();
    }

    const std::uint32_t epilogueOk = as.createLabel();
    const std::uint32_t epilogueBudget = as.createLabel();
    const std::uint32_t epilogueOverflow = as.createLabel();
    const std::uint32_t epilogueDivZero = as.createLabel();
    const std::uint32_t epilogueTypeErr = as.createLabel();
    const std::uint32_t epilogueHelperErr = as.createLabel();
    const std::uint32_t sharedPopRet = as.createLabel();

    std::vector<ColdTrapStub> coldStubs;
    coldStubs.reserve(nInst);

    const auto addColdStub = [&](
        ColdTrapStub::Kind kind,
        std::uint32_t flatIdx,
        std::int32_t restoreBudget = 0,
        std::uint32_t resumeLabel = 0,
        std::uint32_t altResumeLabel = 0) {
        const std::uint32_t lbl = as.createLabel();
        coldStubs.push_back(ColdTrapStub{
            kind, lbl, resumeLabel, altResumeLabel, flatIdx, restoreBudget
        });
        return lbl;
    };

    // Unified AAPCS64 / Apple Darwin ARM64 / Windows ARM64 Prologue (16-byte aligned 64B frame)
    as.subSp(64);
    as.stp64Sp(A64Reg::X29, A64Reg::X30, 0);
    as.movFpFromSp();
    as.stp64Sp(A64Reg::X19, A64Reg::X20, 16);
    as.stp64Sp(A64Reg::X21, A64Reg::X22, 32);
    as.stp64Sp(A64Reg::X23, A64Reg::X24, 48);

    // X0 holds JitAbiContext* on macOS/iOS, Android/Linux, and Windows ARM64
    as.movReg64(A64Reg::X19, A64Reg::X0);
    as.loadMem64(A64Reg::X20, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, frameBase)));
    as.loadMem64(A64Reg::X21, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)));
    as.loadMem64(A64Reg::X22, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)));

    if (execFunc.entryStreamOffset != 0) {
        as.b(instLabels[execFunc.entryStreamOffset]);
    }

    const auto slotOff = [](std::uint16_t slot) noexcept -> std::int32_t {
        return static_cast<std::int32_t>(slot) * 16;
    };

    const auto emitBudgetDeduct = [&](std::uint32_t idx, std::uint8_t span, bool mayNeedPartialStep) {
        as.subsImm64(A64Reg::X21, A64Reg::X21, span);
        if (span > 1u && mayNeedPartialStep) {
            const auto stub = addColdStub(
                ColdTrapStub::Kind::BudgetPartialSuper, idx, static_cast<std::int32_t>(span));
            as.bcc(A64Cond::LO, stub);
        } else {
            const auto stub = addColdStub(ColdTrapStub::Kind::BudgetSimple, idx);
            as.bcc(A64Cond::LO, stub);
        }
    };

    const auto emitCheckTagOrTrap = [&](
        std::uint16_t slot,
        std::uint8_t expectedTag,
        std::uint32_t idx,
        std::int32_t restoreBudgetAfterFail) {
        as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(slot) + 15);
        as.cmpImm32(A64Reg::X6, expectedTag);
        const auto stub = addColdStub(ColdTrapStub::Kind::TypeMismatch, idx, restoreBudgetAfterFail);
        as.bcc(A64Cond::NE, stub);
    };

    const auto emitHelperCallInline = [&](
        std::uint32_t (*helperFn)(JitAbiContext*, std::uint32_t),
        std::uint32_t flatIdx) {
        as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
        as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
        as.movReg64(A64Reg::X0, A64Reg::X19);
        as.movImm32(A64Reg::X1, flatIdx);
        as.movImm64(A64Reg::X16, fnPtrAddress(helperFn));
        as.blr(A64Reg::X16);
        as.loadMem64(A64Reg::X20, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, frameBase)));
        as.loadMem64(A64Reg::X21, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)));
        as.loadMem64(A64Reg::X22, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)));
        as.cmpImm32(A64Reg::X0, 0);
        as.bcc(A64Cond::NE, epilogueHelperErr);
    };

    // Copies `slotCount` 16-byte Value slots from pointer in X3 to pointer in X4 using 128-bit Q0
    const auto emitCopySlots128 = [&](std::uint16_t slotCount) {
        if (slotCount <= 8u) {
            for (std::uint16_t k = 0; k < slotCount; ++k) {
                as.ldrQ(0, A64Reg::X3, static_cast<std::int32_t>(k) * 16);
                as.strQ(A64Reg::X4, static_cast<std::int32_t>(k) * 16, 0);
            }
        } else {
            as.movImm32(A64Reg::X5, slotCount);
            const std::uint32_t loopLbl = as.createLabel();
            as.bind(loopLbl);
            as.ldrQ(0, A64Reg::X3, 0);
            as.strQ(A64Reg::X4, 0, 0);
            as.addImm64(A64Reg::X3, A64Reg::X3, 16);
            as.addImm64(A64Reg::X4, A64Reg::X4, 16);
            as.subsImm32(A64Reg::X5, A64Reg::X5, 1u);
            as.bcc(A64Cond::NE, loopLbl);
        }
    };

    // Operates on W0 (lhs) and W1 (rhs); returns kTagInt64 (result in X0) or kTagBool (0/1 in W0)
    const auto emitQuickIntOpCore = [&](
        runtime::detail::QuickOpcode qop,
        bool checked,
        std::uint32_t idx,
        std::int32_t unexecAfterTrap) -> std::uint8_t {
        using QO = runtime::detail::QuickOpcode;
        switch (qop) {
        case QO::AddInt:
            if (checked) {
                as.addsReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.bcc(A64Cond::VS, ovfStub);
            } else {
                as.addReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            }
            as.sxtw(A64Reg::X0, A64Reg::X0);
            return kTagInt64;
        case QO::SubtractInt:
            if (checked) {
                as.subsReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.bcc(A64Cond::VS, ovfStub);
            } else {
                as.subReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            }
            as.sxtw(A64Reg::X0, A64Reg::X0);
            return kTagInt64;
        case QO::MultiplyInt:
            as.smullReg(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            as.sxtw(A64Reg::X2, A64Reg::X0);
            if (checked) {
                as.cmpReg64(A64Reg::X0, A64Reg::X2);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
                as.bcc(A64Cond::NE, ovfStub);
            } else {
                as.movReg64(A64Reg::X0, A64Reg::X2);
            }
            return kTagInt64;
        case QO::DivideInt: {
            as.cmpImm32(A64Reg::X1, 0);
            const auto div0Stub = addColdStub(ColdTrapStub::Kind::DivisionByZero, idx, unexecAfterTrap);
            as.bcc(A64Cond::EQ, div0Stub);
            const std::uint32_t doDiv = as.createLabel();
            as.cmpImm32(A64Reg::X0, std::numeric_limits<std::int32_t>::min());
            as.bcc(A64Cond::NE, doDiv);
            as.cmpImm32(A64Reg::X1, -1);
            const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, idx, unexecAfterTrap);
            as.bcc(A64Cond::EQ, ovfStub);
            as.bind(doDiv);
            as.sdivReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            as.sxtw(A64Reg::X0, A64Reg::X0);
            return kTagInt64;
        }
        case QO::RemainderInt: {
            as.cmpImm32(A64Reg::X1, 0);
            const auto div0Stub = addColdStub(ColdTrapStub::Kind::DivisionByZero, idx, unexecAfterTrap);
            as.bcc(A64Cond::EQ, div0Stub);
            const std::uint32_t doRem = as.createLabel();
            const std::uint32_t remDone = as.createLabel();
            as.cmpImm32(A64Reg::X0, std::numeric_limits<std::int32_t>::min());
            as.bcc(A64Cond::NE, doRem);
            as.cmpImm32(A64Reg::X1, -1);
            as.bcc(A64Cond::NE, doRem);
            as.movz32(A64Reg::X0, 0);
            as.b(remDone);
            as.bind(doRem);
            as.sdivReg32(A64Reg::X2, A64Reg::X0, A64Reg::X1);
            as.msubReg32(A64Reg::X0, A64Reg::X2, A64Reg::X1, A64Reg::X0);
            as.sxtw(A64Reg::X0, A64Reg::X0);
            as.bind(remDone);
            return kTagInt64;
        }
        default:
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.cset32(A64Reg::X0, quickCmpToA64Cond(qop));
            return kTagBool;
        }
    };

    const auto emitWriteTaggedResult = [&](std::uint16_t slot, std::uint8_t tag) {
        const std::int32_t off = slotOff(slot);
        if (tag == kTagBool) {
            as.storeMem8Reg(A64Reg::X20, off, A64Reg::X0);
            as.storeMem8Imm(A64Reg::X20, off + 15, kTagBool);
        } else {
            as.storeMem64(A64Reg::X20, off, A64Reg::X0);
            as.storeMem8Imm(A64Reg::X20, off + 15, tag);
        }
    };

    const auto emitCondBranchTargets = [&](
        A64Cond trueCc,
        std::uint32_t idx,
        std::uint32_t trueOff,
        std::uint32_t falseOff) {
        if (falseOff == idx + 1u) {
            as.bcc(trueCc, instLabels[trueOff]);
        } else if (trueOff == idx + 1u) {
            as.bcc(invertA64Cond(trueCc), instLabels[falseOff]);
        } else {
            as.bcc(trueCc, instLabels[trueOff]);
            as.b(instLabels[falseOff]);
        }
    };

    for (std::size_t idx = 0; idx < nInst; ++idx) {
        as.bind(instLabels[idx]);
        const auto& fi = execFunc.flatStream[idx];
        const auto i32Idx = static_cast<std::uint32_t>(idx);
        const auto unexecAfter = static_cast<std::int32_t>(record.unexecutedAfterOp[idx]);
        using FO = runtime::detail::FlatOpcode;
        using QO = runtime::detail::QuickOpcode;

        switch (fi.flatOpcode) {
        case FO::MoveSlot:
        case FO::AliasStructSlice:
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.dstSlot != fi.src0Slot) {
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
                as.strQ(A64Reg::X20, slotOff(fi.dstSlot), 0);
            }
            break;

        case FO::StoreLocalStructFieldInPlace: {
            const auto targetLeafSlot = static_cast<std::uint32_t>(fi.auxImm32 + fi.aux0Slot);
            if (fi.aux1Slot == 0u) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::EQ, slowStub);
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src1Slot));
                as.strQ(A64Reg::X20, static_cast<std::int32_t>(targetLeafSlot * 16u), 0);
                as.bind(doneLbl);
            } else if (const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot);
                       subSc != nullptr && targetLeafSlot + subSc->flatSlotCount <= execFunc.totalFrameSlots) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 13);
                as.cmpImm32(A64Reg::X6, 0);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 12);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(fi.aux1Slot & 0xFFu));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 14);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>((fi.aux1Slot >> 8u) & 0xFFu));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src1Slot) + 8);
                as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.movz32(A64Reg::X6, 0x40u);
                as.orrReg32(A64Reg::X1, A64Reg::X1, A64Reg::X6);
                as.lsrImm32(A64Reg::X6, A64Reg::X0, 24u);
                as.cmpReg32(A64Reg::X6, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.andLow24Bits32(A64Reg::X0, A64Reg::X0);
                as.loadMem64(A64Reg::X6, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.addReg64(A64Reg::X3, A64Reg::X6, A64Reg::X0, 4u);
                as.addImm64(A64Reg::X4, A64Reg::X20, static_cast<std::int64_t>(targetLeafSlot) * 16LL);
                const std::uint32_t skipCopyLbl = as.createLabel();
                as.cmpReg64(A64Reg::X3, A64Reg::X4);
                as.bcc(A64Cond::EQ, skipCopyLbl);
                emitCopySlots128(subSc->flatSlotCount);
                as.bind(skipCopyLbl);
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::LoadStructSliceField: {
            const std::uint16_t subSid = fi.aux1Slot;
            if (subSid == 0u || fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 13);
                as.cmpImm32(A64Reg::X6, 0);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.movz32(A64Reg::X6, 0x40u);
                as.orrReg32(A64Reg::X1, A64Reg::X1, A64Reg::X6);
                as.lsrImm32(A64Reg::X6, A64Reg::X0, 24u);
                as.cmpReg32(A64Reg::X6, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.andLow24Bits32(A64Reg::X0, A64Reg::X0);
                as.addImm64(A64Reg::X0, A64Reg::X0, fi.aux0Slot);
                as.cmpImm32(A64Reg::X0, static_cast<std::int32_t>(runtime::StructStorageHandle::FrameOffsetMask));
                as.bcc(A64Cond::HI, slowStub);
                as.loadMem64(A64Reg::X6, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                if (subSid == 0u) {
                    as.addReg64(A64Reg::X3, A64Reg::X6, A64Reg::X0, 4u);
                    as.ldrQ(0, A64Reg::X3, 0);
                    as.strQ(A64Reg::X20, slotOff(fi.dstSlot), 0);
                } else {
                    as.lslImm64(A64Reg::X1, A64Reg::X1, 24u);
                    as.orrReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
                    as.movImm64(A64Reg::X2, static_cast<std::uint64_t>(fi.integerImmediate));
                    as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot) + 0, A64Reg::X2);
                    as.storeMem32(A64Reg::X20, slotOff(fi.dstSlot) + 8, A64Reg::X0);
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(subSid & 0xFFu));
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 13, 0u);
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((subSid >> 8u) & 0xFFu));
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                }
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::CopyStructSlice: {
            const std::uint16_t sid = fi.aux0Slot;
            const std::uint32_t dstPayOff = fi.auxImm32;
            const auto* sc = runtime::getFlatStructSchema(sid);
            if (sc != nullptr && dstPayOff + sc->flatSlotCount <= execFunc.totalFrameSlots) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 13);
                as.cmpImm32(A64Reg::X6, 0);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 12);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(sid & 0xFFu));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 14);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>((sid >> 8u) & 0xFFu));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.movz32(A64Reg::X6, 0x40u);
                as.orrReg32(A64Reg::X1, A64Reg::X1, A64Reg::X6);
                as.lsrImm32(A64Reg::X6, A64Reg::X0, 24u);
                as.cmpReg32(A64Reg::X6, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.andLow24Bits32(A64Reg::X0, A64Reg::X0);
                as.loadMem32(A64Reg::X8, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, frameOffset32)));
                if (dstPayOff != 0u) {
                    as.movImm32(A64Reg::X6, dstPayOff);
                    as.addReg32(A64Reg::X8, A64Reg::X8, A64Reg::X6);
                }
                as.cmpImm32(A64Reg::X8, static_cast<std::int32_t>(runtime::StructStorageHandle::FrameOffsetMask));
                as.bcc(A64Cond::HI, slowStub);
                as.loadMem64(A64Reg::X6, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.movReg64(A64Reg::X7, A64Reg::X1); // save (0x40 | bankId)
                as.addReg64(A64Reg::X3, A64Reg::X6, A64Reg::X0, 4u);
                as.addImm64(A64Reg::X4, A64Reg::X20, static_cast<std::int64_t>(dstPayOff) * 16LL);
                const std::uint32_t skipCopyLbl = as.createLabel();
                as.cmpReg64(A64Reg::X3, A64Reg::X4);
                as.bcc(A64Cond::EQ, skipCopyLbl);
                emitCopySlots128(sc->flatSlotCount);
                as.bind(skipCopyLbl);
                as.lslImm64(A64Reg::X7, A64Reg::X7, 24u);
                as.movReg64(A64Reg::X0, A64Reg::X8);
                as.orrReg32(A64Reg::X0, A64Reg::X0, A64Reg::X7);
                as.movImm64(A64Reg::X2, static_cast<std::uint64_t>(sc->structTypeId));
                as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot) + 0, A64Reg::X2);
                as.storeMem32(A64Reg::X20, slotOff(fi.dstSlot) + 8, A64Reg::X0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 13, 0u);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::WritebackLocalStackRef:
            emitBudgetDeduct(i32Idx, fi.span, false);
            break;

        case FO::StoreStackRefStructFieldInPlace: {
            if (fi.aux1Slot == 0u) {
                const std::uint32_t doneLbl = as.createLabel();
                const auto slowStub = addColdStub(ColdTrapStub::Kind::FlatStructSlow, i32Idx, 0, doneLbl);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagObjectRef);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                as.cmpImm32(A64Reg::X0, static_cast<std::int32_t>(runtime::ObjectRef::StackSlotRefHeapId));
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                as.loadMem32(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 4);
                as.cmpReg32(A64Reg::X1, A64Reg::X6);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, static_cast<std::int32_t>(runtime::ValueTag::Struct));
                as.bcc(A64Cond::EQ, slowStub);
                as.loadMem64(A64Reg::X6, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, valuesBase)));
                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 0);
                as.addImm64(A64Reg::X0, A64Reg::X0, fi.aux0Slot);
                as.addReg64(A64Reg::X3, A64Reg::X6, A64Reg::X0, 4u);
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src1Slot));
                as.strQ(A64Reg::X3, 0, 0);
                as.bind(doneLbl);
            } else {
                emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            }
            break;
        }

        case FO::InitStructSlice:
        case FO::StoreStructSliceField:
        case FO::InitStackRefCell:
        case FO::BindLocalStackRef:
        case FO::LoadStackRefCell:
        case FO::CheckNotNullFast:
        case FO::StoreStackRefCell:
        case FO::NewObjectFast:
        case FO::LoadFieldExactFast:
        case FO::LoadFieldFast:
        case FO::StoreFieldExactFast:
        case FO::StoreFieldFast:
        case FO::GenericBytecode:
            emitHelperCallInline(&jitFlatStructAndStackRefHelper, i32Idx);
            break;

        case FO::SuperMoveParamStoreLocal:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.strQ(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.strQ(A64Reg::X20, slotOff(fi.storeSlot), 0);
            break;

        case FO::SuperMoveParamStoreLocalElided:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.strQ(A64Reg::X20, slotOff(fi.storeSlot), 0);
            break;

        case FO::ConstantInt:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem64Imm(
                A64Reg::X20,
                slotOff(fi.dstSlot),
                static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(fi.integerImmediate))));
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagInt64);
            break;

        case FO::ConstantLong:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem64Imm(A64Reg::X20, slotOff(fi.dstSlot), static_cast<std::uint64_t>(fi.integerImmediate));
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagLong);
            break;

        case FO::ConstantBool:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot), fi.integerImmediate != 0 ? 1u : 0u);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagBool);
            break;

        case FO::ConstantDouble:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.storeMem64Imm(A64Reg::X20, slotOff(fi.dstSlot), static_cast<std::uint64_t>(fi.integerImmediate));
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::SuperConstFloat: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            std::uint32_t fBits = 0;
            std::memcpy(&fBits, &fi.integerImmediate, 4);
            as.storeMem32Imm(A64Reg::X20, slotOff(fi.dstSlot), fBits);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::SuperConstFloatStoreLocal: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            std::uint32_t fBits = 0;
            std::memcpy(&fBits, &fi.integerImmediate, 4);
            as.storeMem32Imm(A64Reg::X20, slotOff(fi.storeSlot), fBits);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.storeSlot) + 15, kTagFloat);
            break;
        }

        case FO::AddIntChecked:
        case FO::AddIntUnchecked:
        case FO::SubIntChecked:
        case FO::SubIntUnchecked:
        case FO::MulIntChecked:
        case FO::MulIntUnchecked:
        case FO::DivInt:
        case FO::RemInt:
        case FO::LessInt:
        case FO::LessOrEqualInt:
        case FO::GreaterInt:
        case FO::GreaterOrEqualInt:
        case FO::EqualInt:
        case FO::NotEqualInt:
        case FO::AddIntTrustedChecked:
        case FO::AddIntTrustedUnchecked:
        case FO::SubIntTrustedChecked:
        case FO::SubIntTrustedUnchecked:
        case FO::MulIntTrustedChecked:
        case FO::MulIntTrustedUnchecked:
        case FO::LessIntTrusted: {
            const bool isTrusted =
                fi.flatOpcode >= FO::AddIntTrustedChecked &&
                fi.flatOpcode <= FO::LessIntTrusted;
            const bool isChecked =
                fi.flatOpcode == FO::AddIntChecked ||
                fi.flatOpcode == FO::SubIntChecked ||
                fi.flatOpcode == FO::MulIntChecked ||
                fi.flatOpcode == FO::AddIntTrustedChecked ||
                fi.flatOpcode == FO::SubIntTrustedChecked ||
                fi.flatOpcode == FO::MulIntTrustedChecked;
            const bool mayTrap =
                isChecked || fi.flatOpcode == FO::DivInt || fi.flatOpcode == FO::RemInt;

            QO qop = QO::AddInt;
            switch (fi.flatOpcode) {
            case FO::AddIntChecked: case FO::AddIntUnchecked:
            case FO::AddIntTrustedChecked: case FO::AddIntTrustedUnchecked:
                qop = QO::AddInt; break;
            case FO::SubIntChecked: case FO::SubIntUnchecked:
            case FO::SubIntTrustedChecked: case FO::SubIntTrustedUnchecked:
                qop = QO::SubtractInt; break;
            case FO::MulIntChecked: case FO::MulIntUnchecked:
            case FO::MulIntTrustedChecked: case FO::MulIntTrustedUnchecked:
                qop = QO::MultiplyInt; break;
            case FO::DivInt: qop = QO::DivideInt; break;
            case FO::RemInt: qop = QO::RemainderInt; break;
            case FO::LessInt: case FO::LessIntTrusted: qop = QO::LessInt; break;
            case FO::LessOrEqualInt: qop = QO::LessOrEqualInt; break;
            case FO::GreaterInt: qop = QO::GreaterInt; break;
            case FO::GreaterOrEqualInt: qop = QO::GreaterOrEqualInt; break;
            case FO::EqualInt: qop = QO::EqualInt; break;
            case FO::NotEqualInt: qop = QO::NotEqualInt; break;
            default: break;
            }

            emitBudgetDeduct(i32Idx, fi.span, mayTrap && fi.span > 1u);
            if (!isTrusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem32(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            const std::uint8_t outTag = emitQuickIntOpCore(qop, isChecked, i32Idx, unexecAfter);
            emitWriteTaggedResult(fi.dstSlot, outTag);
            break;
        }

        case FO::NegateIntUnchecked:
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.subReg32(A64Reg::X0, A64Reg::XZR, A64Reg::X0);
            as.sxtw(A64Reg::X0, A64Reg::X0);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;

        case FO::LogicalNot:
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagBool, i32Idx, unexecAfter);
            as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.movz32(A64Reg::X1, 1u);
            as.eorReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;

        case FO::SuperBinaryLocalIntConst:
        case FO::SuperBinaryLocalIntConstStore:
        case FO::SuperBinaryLocalIntConstStoreElided:
        case FO::SuperBinaryLocalIntConstStoreJump: {
            const bool isElided = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreElided);
            const bool isJump = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump);
            const bool hasStore = (fi.flatOpcode == FO::SuperBinaryLocalIntConstStore);
            const bool trusted0 = (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;

            emitBudgetDeduct(i32Idx, fi.span, true);
            if (!trusted0) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            if (!isElided && !isJump) {
                as.sxtw(A64Reg::X2, A64Reg::X0);
                as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot), A64Reg::X2);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagInt64);
                as.storeMem64Imm(
                    A64Reg::X20,
                    slotOff(fi.aux0Slot),
                    static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(fi.integerImmediate))));
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux0Slot) + 15, kTagInt64);
            }
            as.movImm32(A64Reg::X1, static_cast<std::uint32_t>(static_cast<std::int32_t>(fi.integerImmediate)));
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, unexecAfter);
            if (isJump) {
                emitWriteTaggedResult(fi.dstSlot, outTag);
                const std::int64_t brInc =
                    ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2 : 1;
                as.addImm64(A64Reg::X22, A64Reg::X22, brInc);
                as.b(instLabels[fi.trueTargetOffset]);
            } else if (isElided) {
                emitWriteTaggedResult(fi.storeSlot, outTag);
            } else {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
                if (hasStore) {
                    emitWriteTaggedResult(fi.storeSlot, outTag);
                }
            }
            break;
        }

        case FO::SuperBinaryLocalIntLocal:
        case FO::SuperBinaryLocalIntLocalStore:
        case FO::SuperBinaryLocalIntLocalStoreElided:
        case FO::SuperBinaryLocalIntLocalStoreJump:
        case FO::SuperBinaryLocalIntReg:
        case FO::SuperBinaryLocalIntRegStore:
        case FO::SuperBinaryLocalIntRegStoreElided:
        case FO::SuperBinaryLocalIntRegStoreJump: {
            const bool isLocalRhs =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocal ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump;
            const bool isElided =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreElided;
            const bool isJump =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreJump;
            const bool hasStore =
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStore;
            const bool trustedBoth =
                (fi.arithFlags & (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                                  runtime::detail::FlatInstruction::FlagTrustedSrc1)) ==
                (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                 runtime::detail::FlatInstruction::FlagTrustedSrc1);

            emitBudgetDeduct(i32Idx, fi.span, true);
            if (!trustedBoth) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, unexecAfter);
            }
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem32(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            if (!isElided && !isJump) {
                as.sxtw(A64Reg::X2, A64Reg::X0);
                as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot), A64Reg::X2);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagInt64);
                if (isLocalRhs) {
                    as.sxtw(A64Reg::X2, A64Reg::X1);
                    as.storeMem64(A64Reg::X20, slotOff(fi.aux0Slot), A64Reg::X2);
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux0Slot) + 15, kTagInt64);
                }
            }
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, unexecAfter);
            if (isJump) {
                emitWriteTaggedResult(fi.dstSlot, outTag);
                const std::int64_t brInc =
                    ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2 : 1;
                as.addImm64(A64Reg::X22, A64Reg::X22, brInc);
                as.b(instLabels[fi.trueTargetOffset]);
            } else if (isElided) {
                emitWriteTaggedResult(fi.storeSlot, outTag);
            } else {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
                if (hasStore) {
                    emitWriteTaggedResult(fi.storeSlot, outTag);
                }
            }
            break;
        }

        case FO::SuperBinaryLocalIntRegChainStore:
        case FO::SuperBinaryLocalIntRegChainStoreElided: {
            const bool isElided = (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStoreElided);
            emitBudgetDeduct(i32Idx, fi.span, true);
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 2);
            emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, 2);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem32(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            if (!isElided) {
                as.sxtw(A64Reg::X2, A64Reg::X0);
                as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot), A64Reg::X2);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagInt64);
            }
            const std::uint8_t midTag = emitQuickIntOpCore(fi.binaryOpcode1, fi.checked1(), i32Idx, 2);
            if (!isElided) {
                emitWriteTaggedResult(fi.aux0Slot, midTag);
            }
            emitCheckTagOrTrap(fi.src2Slot, kTagInt64, i32Idx, 1);
            as.loadMem32(A64Reg::X1, A64Reg::X20, slotOff(fi.src2Slot));
            const std::uint8_t outTag = emitQuickIntOpCore(fi.binaryOpcode2, fi.checked2(), i32Idx, 1);
            if (!isElided) {
                emitWriteTaggedResult(fi.aux1Slot, outTag);
            }
            emitWriteTaggedResult(fi.storeSlot, outTag);
            break;
        }

        case FO::SuperLessLocalConstBranch: {
            const bool trusted0 = (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (!trusted0) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 1);
            }
            as.addImm64(A64Reg::X22, A64Reg::X22, 1);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.cmpImm64(A64Reg::X0, static_cast<std::int64_t>(static_cast<std::int32_t>(fi.integerImmediate)));
            emitCondBranchTargets(
                quickCmpToA64Cond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::SuperLessLocalRegBranch: {
            const bool trustedBoth =
                (fi.arithFlags & (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                                  runtime::detail::FlatInstruction::FlagTrustedSrc1)) ==
                (runtime::detail::FlatInstruction::FlagTrustedSrc0 |
                 runtime::detail::FlatInstruction::FlagTrustedSrc1);
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (!trustedBoth) {
                emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, 1);
                emitCheckTagOrTrap(fi.src1Slot, kTagInt64, i32Idx, 1);
            }
            as.addImm64(A64Reg::X22, A64Reg::X22, 1);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            emitCondBranchTargets(
                quickCmpToA64Cond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::SuperLessLocalArrayLengthBranch: {
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src1Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLengthBranchSlow,
                i32Idx,
                0,
                instLabels[fi.trueTargetOffset],
                instLabels[fi.falseTargetOffset]);

            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagInt64);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagObjectRef);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src1Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src1Slot) + 8);
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.addImm64(A64Reg::X22, A64Reg::X22, 1);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 24);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            emitCondBranchTargets(
                quickCmpToA64Cond(fi.binaryOpcode1), i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;
        }

        case FO::ArrayLengthFast: {
            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLengthSlow, i32Idx, 0, doneLbl);

            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagObjectRef);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem32(A64Reg::X0, A64Reg::X19, cacheDisp + 24);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            as.bind(doneLbl);
            break;
        }

        case FO::NewArrayFast:
            emitHelperCallInline(&jitNewArrayHelper, i32Idx);
            break;

        case FO::LoadElementIntFast:
        case FO::LoadElementTypedFast: {
            const bool isTyped = (fi.flatOpcode == FO::LoadElementTypedFast);
            const auto elemType = isTyped
                ? static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1))
                : semantic::PrimitiveType::Int;
            const auto elemTid = isTyped
                ? static_cast<semantic::SymbolId>(fi.integerImmediate)
                : semantic::SymbolId{0};

            std::uint8_t fastEncoding = 0u;
            if (elemTid == 0) {
                if (elemType == semantic::PrimitiveType::Int) fastEncoding = kPackedInt32;
                else if (elemType == semantic::PrimitiveType::Double) fastEncoding = kPackedFloat64;
                else if (elemType == semantic::PrimitiveType::Bool) fastEncoding = kPackedBool;
            }

            if (fastEncoding == 0u) {
                if (isTyped &&
                    elemType == semantic::PrimitiveType::Struct &&
                    execFunc.hasStructPayloads &&
                    fi.dstSlot < execFunc.slotStructSchemaIds.size() &&
                    execFunc.slotStructSchemaIds[fi.dstSlot] != 0u) {
                    const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
                    const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
                    const auto* sc = runtime::getFlatStructSchema(sid);
                    if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg &&
                        static_cast<std::uint32_t>(payOff) + sc->flatSlotCount <= execFunc.totalFrameSlots) {
                        const std::uint32_t doneLbl = as.createLabel();
                        const std::int32_t cacheDisp =
                            static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                            static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
                        const auto slowStub = addColdStub(
                            ColdTrapStub::Kind::ArrayLoadSlow, i32Idx, 0, doneLbl);

                        as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                        as.cmpImm32(A64Reg::X6, kTagObjectRef);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
                        as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
                        as.cmpReg64(A64Reg::X0, A64Reg::X1);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                        as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
                        as.cmpReg32(A64Reg::X0, A64Reg::X1);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 13);
                        as.cmpImm32(A64Reg::X6, 2);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem16ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 30);
                        as.cmpImm32(A64Reg::X6, sid);
                        as.bcc(A64Cond::NE, slowStub);

                        as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                        as.cmpImm32(A64Reg::X6, kTagInt64);
                        as.bcc(A64Cond::NE, slowStub);
                        as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
                        as.loadMem32(A64Reg::X2, A64Reg::X19, cacheDisp + 24);
                        as.cmpReg64(A64Reg::X1, A64Reg::X2);
                        as.bcc(A64Cond::HS, slowStub);

                        emitBudgetDeduct(i32Idx, fi.span, false);
                        as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                        as.movImm64(A64Reg::X6, static_cast<std::uint64_t>(sc->flatSlotCount) * 16ULL);
                        as.mulReg64(A64Reg::X0, A64Reg::X1, A64Reg::X6);
                        as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X0, 0);
                        as.addImm64(A64Reg::X4, A64Reg::X20, static_cast<std::int64_t>(payOff) * 16LL);
                        emitCopySlots128(sc->flatSlotCount);

                        as.loadMem64(A64Reg::X2, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, session)));
                        as.loadMem8ZeroExt(A64Reg::X1, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, flatBankId)));
                        as.movz32(A64Reg::X6, 0x40u);
                        as.orrReg32(A64Reg::X1, A64Reg::X1, A64Reg::X6);
                        as.lslImm64(A64Reg::X1, A64Reg::X1, 24u);
                        as.loadMem32(A64Reg::X0, A64Reg::X2, static_cast<std::int32_t>(offsetof(JitRuntimeSession, frameOffset32)));
                        if (payOff != 0u) {
                            as.movImm32(A64Reg::X6, static_cast<std::uint32_t>(payOff));
                            as.addReg32(A64Reg::X0, A64Reg::X0, A64Reg::X6);
                        }
                        as.orrReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
                        as.movImm64(A64Reg::X2, static_cast<std::uint64_t>(elemTid));
                        as.storeMem64(A64Reg::X20, slotOff(fi.dstSlot) + 0, A64Reg::X2);
                        as.storeMem32(A64Reg::X20, slotOff(fi.dstSlot) + 8, A64Reg::X0);
                        as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 12, static_cast<std::uint8_t>(sid & 0xFFu));
                        as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 13, 0u);
                        as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 14, static_cast<std::uint8_t>((sid >> 8u) & 0xFFu));
                        as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, static_cast<std::uint8_t>(runtime::ValueTag::Struct));
                        as.bind(doneLbl);
                        break;
                    }
                }
                emitHelperCallInline(&jitLoadElementHelper, i32Idx);
                break;
            }

            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayLoadSlow, i32Idx, 0, doneLbl);

            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagObjectRef);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 14);
            as.cmpImm32(A64Reg::X6, fastEncoding);
            as.bcc(A64Cond::NE, slowStub);

            const bool noBce = (fi.arithFlags & runtime::detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u;
            const bool trustedIdx = !isTyped &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            if (!noBce && !trustedIdx) {
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagInt64);
                as.bcc(A64Cond::NE, slowStub);
            }
            as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            if (!noBce) {
                as.loadMem32(A64Reg::X2, A64Reg::X19, cacheDisp + 24);
                as.cmpReg64(A64Reg::X1, A64Reg::X2);
                as.bcc(A64Cond::HS, slowStub);
            }

            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
            if (fastEncoding == kPackedInt32) {
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 2u);
                as.loadMem32SignExt64(A64Reg::X0, A64Reg::X3, 0);
                emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            } else if (fastEncoding == kPackedFloat64) {
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 3u);
                as.ldrD(0, A64Reg::X3, 0);
                as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            } else {
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 0u);
                as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X3, 0);
                emitWriteTaggedResult(fi.dstSlot, kTagBool);
            }
            as.bind(doneLbl);
            break;
        }

        case FO::StoreElementStructFieldInPlace: {
            const auto rootSid = static_cast<std::uint16_t>(fi.auxImm32);
            const auto* rootSc = runtime::getFlatStructSchema(rootSid);
            if (fi.aux1Slot == 0u && rootSc != nullptr && fi.aux0Slot < rootSc->flatSlotCount) {
                const std::uint32_t doneLbl = as.createLabel();
                const std::int32_t cacheDisp =
                    static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                    static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
                const auto slowStub = addColdStub(
                    ColdTrapStub::Kind::ArrayStoreSlow, i32Idx, 0, doneLbl);

                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagObjectRef);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
                as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
                as.cmpReg64(A64Reg::X0, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
                as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
                as.cmpReg32(A64Reg::X0, A64Reg::X1);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 13);
                as.cmpImm32(A64Reg::X6, 2);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem16ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 30);
                as.cmpImm32(A64Reg::X6, rootSid);
                as.bcc(A64Cond::NE, slowStub);

                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagInt64);
                as.bcc(A64Cond::NE, slowStub);
                as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
                as.loadMem32(A64Reg::X2, A64Reg::X19, cacheDisp + 24);
                as.cmpReg64(A64Reg::X1, A64Reg::X2);
                as.bcc(A64Cond::HS, slowStub);

                as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(fi.src2Slot) + 15);
                as.cmpImm32(A64Reg::X0, 0);
                as.bcc(A64Cond::EQ, slowStub);
                as.cmpImm32(A64Reg::X0, static_cast<std::int32_t>(runtime::ValueTag::FirstTraceable));
                as.bcc(A64Cond::HS, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                as.movImm64(A64Reg::X6, rootSc->flatSlotCount);
                as.mulReg64(A64Reg::X0, A64Reg::X1, A64Reg::X6);
                as.addImm64(A64Reg::X0, A64Reg::X0, fi.aux0Slot);
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X0, 4u);
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src2Slot));
                as.strQ(A64Reg::X3, 0, 0);
                as.bind(doneLbl);
                break;
            }
            emitHelperCallInline(&jitStoreElementHelper, i32Idx);
            break;
        }

        case FO::StoreElementStructFast:
        case FO::StoreElementObjFast:
            emitHelperCallInline(&jitStoreElementHelper, i32Idx);
            break;

        case FO::StoreElementIntFast:
        case FO::StoreElementPrimFast: {
            const bool isPrim = (fi.flatOpcode == FO::StoreElementPrimFast);
            const auto elemType = isPrim
                ? static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1))
                : semantic::PrimitiveType::Int;

            std::uint8_t fastEncoding = 0u;
            if (elemType == semantic::PrimitiveType::Int) fastEncoding = kPackedInt32;
            else if (elemType == semantic::PrimitiveType::Double) fastEncoding = kPackedFloat64;
            else if (elemType == semantic::PrimitiveType::Bool) fastEncoding = kPackedBool;

            if (fastEncoding == 0u) {
                emitHelperCallInline(&jitStoreElementHelper, i32Idx);
                break;
            }

            const std::uint32_t doneLbl = as.createLabel();
            const std::int32_t cacheDisp =
                static_cast<std::int32_t>(offsetof(JitAbiContext, arrayCache)) +
                static_cast<std::int32_t>(fi.src0Slot & 3u) * 64;
            const auto slowStub = addColdStub(
                ColdTrapStub::Kind::ArrayStoreSlow, i32Idx, 0, doneLbl);

            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src0Slot) + 15);
            as.cmpImm32(A64Reg::X6, kTagObjectRef);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X1, A64Reg::X19, cacheDisp + 0);
            as.cmpReg64(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot) + 8);
            as.loadMem32(A64Reg::X1, A64Reg::X19, cacheDisp + 8);
            as.cmpReg32(A64Reg::X0, A64Reg::X1);
            as.bcc(A64Cond::NE, slowStub);
            as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X19, cacheDisp + 14);
            as.cmpImm32(A64Reg::X6, fastEncoding);
            as.bcc(A64Cond::NE, slowStub);

            const bool noBce = (fi.arithFlags & runtime::detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u;
            const bool trustedIdx = !isPrim &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc0) != 0u;
            const bool trustedVal = !isPrim &&
                (fi.arithFlags & runtime::detail::FlatInstruction::FlagTrustedSrc1) != 0u;
            if (!noBce && !trustedIdx) {
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src1Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagInt64);
                as.bcc(A64Cond::NE, slowStub);
            }
            as.loadMem64(A64Reg::X1, A64Reg::X20, slotOff(fi.src1Slot));
            if (!noBce) {
                as.loadMem32(A64Reg::X2, A64Reg::X19, cacheDisp + 24);
                as.cmpReg64(A64Reg::X1, A64Reg::X2);
                as.bcc(A64Cond::HS, slowStub);
            }

            if (fastEncoding == kPackedInt32) {
                if (!trustedVal) {
                    as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src2Slot) + 15);
                    as.cmpImm32(A64Reg::X6, kTagInt64);
                    as.bcc(A64Cond::NE, slowStub);
                }
                as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src2Slot));
                as.sxtw(A64Reg::X6, A64Reg::X0);
                as.cmpReg64(A64Reg::X0, A64Reg::X6);
                as.bcc(A64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 2u);
                as.storeMem32(A64Reg::X3, 0, A64Reg::X0);
            } else if (fastEncoding == kPackedFloat64) {
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src2Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagDouble);
                as.bcc(A64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.ldrD(0, A64Reg::X20, slotOff(fi.src2Slot));
                as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 3u);
                as.strD(A64Reg::X3, 0, 0);
            } else {
                as.loadMem8ZeroExt(A64Reg::X6, A64Reg::X20, slotOff(fi.src2Slot) + 15);
                as.cmpImm32(A64Reg::X6, kTagBool);
                as.bcc(A64Cond::NE, slowStub);

                emitBudgetDeduct(i32Idx, fi.span, false);
                as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(fi.src2Slot));
                as.loadMem64(A64Reg::X2, A64Reg::X19, cacheDisp + 16);
                as.addReg64(A64Reg::X3, A64Reg::X2, A64Reg::X1, 0u);
                as.storeMem8Reg(A64Reg::X3, 0, A64Reg::X0);
            }
            as.bind(doneLbl);
            break;
        }

        // Scalar Double ALU & Comparisons
        case FO::AddDouble:
        case FO::SubDouble:
        case FO::MulDouble:
        case FO::DivDouble:
        case FO::AddDoubleTrusted:
        case FO::SubDoubleTrusted:
        case FO::MulDoubleTrusted:
        case FO::DivDoubleTrusted: {
            const bool trusted =
                fi.flatOpcode >= FO::AddDoubleTrusted && fi.flatOpcode <= FO::DivDoubleTrusted;
            if (!trusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, unexecAfter);
            }
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrD(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrD(1, A64Reg::X20, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::SubDouble || fi.flatOpcode == FO::SubDoubleTrusted) as.fsubD(0, 0, 1);
            else if (fi.flatOpcode == FO::MulDouble || fi.flatOpcode == FO::MulDoubleTrusted) as.fmulD(0, 0, 1);
            else if (fi.flatOpcode == FO::DivDouble || fi.flatOpcode == FO::DivDoubleTrusted) as.fdivD(0, 0, 1);
            else as.faddD(0, 0, 1);
            as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;
        }

        case FO::LessDouble:
        case FO::LessOrEqualDouble:
        case FO::GreaterDouble:
        case FO::GreaterOrEqualDouble: {
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrD(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrD(1, A64Reg::X20, slotOff(fi.src1Slot));
            as.fcmpD(0, 1);
            // ARM64 FCMP NZCV: NaN sets N=0,Z=0,C=1,V=1 -> MI(<), LS(<=), GT(>), GE(>=) are all 0 on NaN!
            if (fi.flatOpcode == FO::LessDouble) as.cset32(A64Reg::X0, A64Cond::MI);
            else if (fi.flatOpcode == FO::LessOrEqualDouble) as.cset32(A64Reg::X0, A64Cond::LS);
            else if (fi.flatOpcode == FO::GreaterDouble) as.cset32(A64Reg::X0, A64Cond::GT);
            else as.cset32(A64Reg::X0, A64Cond::GE);
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;
        }

        case FO::NegateDouble:
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.movz64(A64Reg::X1, 0x8000u, 3);
            as.eorReg64(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            emitWriteTaggedResult(fi.dstSlot, kTagDouble);
            break;

        case FO::SuperBinaryLocalDoubleConstStore:
        case FO::SuperBinaryLocalDoubleConstStoreElided:
        case FO::SuperBinaryLocalDoubleLocalStore:
        case FO::SuperBinaryLocalDoubleLocalStoreElided: {
            const bool isConst =
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStore ||
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStoreElided;
            const bool isElided =
                fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStoreElided ||
                fi.flatOpcode == FO::SuperBinaryLocalDoubleLocalStoreElided;
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, 1);
            as.ldrD(0, A64Reg::X20, slotOff(fi.src0Slot));
            if (isConst) {
                as.movImm64(A64Reg::X0, static_cast<std::uint64_t>(fi.integerImmediate));
                as.fmovDFromX(1, A64Reg::X0);
            } else {
                emitCheckTagOrTrap(fi.src1Slot, kTagDouble, i32Idx, 1);
                as.ldrD(1, A64Reg::X20, slotOff(fi.src1Slot));
            }
            if (!isElided) {
                as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
                as.strD(A64Reg::X20, slotOff(fi.aux0Slot), 1);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux0Slot) + 15, kTagDouble);
            }
            if (fi.binaryOpcode1 == QO::SubtractDouble) as.fsubD(0, 0, 1);
            else if (fi.binaryOpcode1 == QO::MultiplyDouble) as.fmulD(0, 0, 1);
            else if (fi.binaryOpcode1 == QO::DivideDouble) as.fdivD(0, 0, 1);
            else as.faddD(0, 0, 1);
            if (!isElided) {
                as.strD(A64Reg::X20, slotOff(fi.aux1Slot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux1Slot) + 15, kTagDouble);
            }
            as.strD(A64Reg::X20, slotOff(fi.storeSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.storeSlot) + 15, kTagDouble);
            break;
        }

        // Scalar Float ALU & Superinstructions
        case FO::AddFloat:
        case FO::SubFloat:
        case FO::MulFloat:
        case FO::DivFloat:
        case FO::AddFloatTrusted:
        case FO::SubFloatTrusted:
        case FO::MulFloatTrusted: {
            const bool trusted =
                fi.flatOpcode >= FO::AddFloatTrusted && fi.flatOpcode <= FO::MulFloatTrusted;
            if (!trusted) {
                emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
                emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, unexecAfter);
            }
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrS(1, A64Reg::X20, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::SubFloat || fi.flatOpcode == FO::SubFloatTrusted) as.fsubS(0, 0, 1);
            else if (fi.flatOpcode == FO::MulFloat || fi.flatOpcode == FO::MulFloatTrusted) as.fmulS(0, 0, 1);
            else if (fi.flatOpcode == FO::DivFloat) as.fdivS(0, 0, 1);
            else as.faddS(0, 0, 1);
            as.strS(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::LessFloat:
        case FO::LessOrEqualFloat:
        case FO::GreaterFloat:
        case FO::GreaterOrEqualFloat:
        case FO::EqualFloat:
        case FO::NotEqualFloat: {
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrS(1, A64Reg::X20, slotOff(fi.src1Slot));
            as.fcmpS(0, 1);
            if (fi.flatOpcode == FO::LessFloat) as.cset32(A64Reg::X0, A64Cond::MI);
            else if (fi.flatOpcode == FO::LessOrEqualFloat) as.cset32(A64Reg::X0, A64Cond::LS);
            else if (fi.flatOpcode == FO::GreaterFloat) as.cset32(A64Reg::X0, A64Cond::GT);
            else if (fi.flatOpcode == FO::GreaterOrEqualFloat) as.cset32(A64Reg::X0, A64Cond::GE);
            else if (fi.flatOpcode == FO::EqualFloat) as.cset32(A64Reg::X0, A64Cond::EQ);
            else as.cset32(A64Reg::X0, A64Cond::NE);
            emitWriteTaggedResult(fi.dstSlot, kTagBool);
            break;
        }

        case FO::NegateFloat:
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem32(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.movz32(A64Reg::X1, 0x8000u, 1);
            as.eorReg32(A64Reg::X0, A64Reg::X0, A64Reg::X1);
            as.storeMem32(A64Reg::X20, slotOff(fi.dstSlot), A64Reg::X0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;

        case FO::SuperBinaryLocalFloatLocalStore:
        case FO::SuperBinaryLocalFloatLocalStoreElided:
        case FO::SuperBinaryLocalFloatReg:
        case FO::SuperBinaryLocalFloatRegElided:
        case FO::SuperBinaryLocalFloatRegStore:
        case FO::SuperBinaryLocalFloatRegStoreElided:
        case FO::SuperBinaryRegFloatStoreElided: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, 0);
            emitCheckTagOrTrap(fi.src1Slot, kTagFloat, i32Idx, 0);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.ldrS(1, A64Reg::X20, slotOff(fi.src1Slot));
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
                as.strS(A64Reg::X20, slotOff(fi.dstSlot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
                if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore) {
                    as.strS(A64Reg::X20, slotOff(fi.aux0Slot), 1);
                    as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux0Slot) + 15, kTagFloat);
                }
            }
            if (fi.binaryOpcode1 == QO::SubtractFloat) as.fsubS(0, 0, 1);
            else if (fi.binaryOpcode1 == QO::MultiplyFloat) as.fmulS(0, 0, 1);
            else if (fi.binaryOpcode1 == QO::DivideFloat) as.fdivS(0, 0, 1);
            else as.faddS(0, 0, 1);
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegElided ||
                fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
                as.strS(A64Reg::X20, slotOff(fi.aux1Slot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.aux1Slot) + 15, kTagFloat);
            }
            if (fi.flatOpcode != FO::SuperBinaryLocalFloatReg &&
                fi.flatOpcode != FO::SuperBinaryLocalFloatRegElided) {
                as.strS(A64Reg::X20, slotOff(fi.storeSlot), 0);
                as.storeMem8Imm(A64Reg::X20, slotOff(fi.storeSlot) + 15, kTagFloat);
            }
            break;
        }

        // Fast Scalar Conversions
        case FO::ConvertIntToLongFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            emitWriteTaggedResult(fi.dstSlot, kTagLong);
            break;

        case FO::ConvertIntToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.scvtfDFromX(0, A64Reg::X0);
            as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertLongToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagLong, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.scvtfDFromX(0, A64Reg::X0);
            as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertIntToFloatFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagInt64, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.scvtfSFromX(0, A64Reg::X0);
            as.strS(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;

        case FO::ConvertFloatToDoubleFast:
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.fcvtDFromS(0, 0);
            as.strD(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagDouble);
            break;

        case FO::ConvertDoubleToFloatFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagDouble, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.checked1() && fi.span > 1u);
            as.ldrD(0, A64Reg::X20, slotOff(fi.src0Slot));
            if (fi.checked1()) {
                constexpr double kMaxF = static_cast<double>(std::numeric_limits<float>::max());
                constexpr double kMinF = -kMaxF;
                std::uint64_t maxBits = 0, minBits = 0;
                std::memcpy(&maxBits, &kMaxF, 8);
                std::memcpy(&minBits, &kMinF, 8);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
                as.movImm64(A64Reg::X0, minBits);
                as.fmovDFromX(1, A64Reg::X0);
                as.fcmpD(0, 1);
                // Trap if unordered (VS) or d0 < minF (MI) -> !(d0 >= minF) == invert(GE) == LT!
                as.bcc(A64Cond::LT, ovfStub);
                as.movImm64(A64Reg::X0, maxBits);
                as.fmovDFromX(1, A64Reg::X0);
                as.fcmpD(0, 1);
                // Trap if d0 > maxF (GT)
                as.bcc(A64Cond::GT, ovfStub);
            }
            as.fcvtSFromD(0, 0);
            as.strS(A64Reg::X20, slotOff(fi.dstSlot), 0);
            as.storeMem8Imm(A64Reg::X20, slotOff(fi.dstSlot) + 15, kTagFloat);
            break;
        }

        case FO::ConvertFloatToIntFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagFloat, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.span > 1u);
            as.ldrS(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.fcvtDFromS(0, 0);
            constexpr double kMinI = static_cast<double>(std::numeric_limits<std::int32_t>::min());
            constexpr double kMaxI = static_cast<double>(std::numeric_limits<std::int32_t>::max());
            std::uint64_t minBits = 0, maxBits = 0;
            std::memcpy(&minBits, &kMinI, 8);
            std::memcpy(&maxBits, &kMaxI, 8);
            const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
            as.movImm64(A64Reg::X0, minBits);
            as.fmovDFromX(1, A64Reg::X0);
            as.fcmpD(0, 1);
            as.bcc(A64Cond::LT, ovfStub);
            as.movImm64(A64Reg::X0, maxBits);
            as.fmovDFromX(1, A64Reg::X0);
            as.fcmpD(0, 1);
            as.bcc(A64Cond::GT, ovfStub);
            as.fcvtzsXFromD(A64Reg::X0, 0);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;
        }

        case FO::ConvertLongToIntFast: {
            emitCheckTagOrTrap(fi.src0Slot, kTagLong, i32Idx, unexecAfter);
            emitBudgetDeduct(i32Idx, fi.span, fi.checked1() && fi.span > 1u);
            as.loadMem64(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.sxtw(A64Reg::X1, A64Reg::X0);
            if (fi.checked1()) {
                as.cmpReg64(A64Reg::X0, A64Reg::X1);
                const auto ovfStub = addColdStub(ColdTrapStub::Kind::IntegerOverflow, i32Idx, unexecAfter);
                as.bcc(A64Cond::NE, ovfStub);
            }
            as.movReg64(A64Reg::X0, A64Reg::X1);
            emitWriteTaggedResult(fi.dstSlot, kTagInt64);
            break;
        }

        case FO::InlineCallEnter: {
            emitHelperCallInline(&jitInlineCallEnterHelper, i32Idx);
            const std::size_t fastTarget = idx + 1u + fi.inlineArgCount;
            if (fi.inlineArgCount > 0u && fastTarget < nInst) {
                as.loadMem32(
                    A64Reg::X0,
                    A64Reg::X19,
                    static_cast<std::int32_t>(offsetof(JitAbiContext, nextFlatOffsetOut)));
                as.cmpImm32(A64Reg::X0, 0);
                as.bcc(A64Cond::NE, instLabels[fastTarget]);
            }
            break;
        }

        case FO::InlineReturnVoid:
        case FO::InlineReturnVoidJump: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.integerImmediate != 0) {
                const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                const auto clearBase = static_cast<std::uint16_t>(packed & 0xFFFFu);
                const auto clearCount = static_cast<std::uint32_t>(packed >> 16u);
                as.veorZero128(0);
                for (std::uint32_t k = 0; k < clearCount; ++k) {
                    as.strQ(A64Reg::X20, slotOff(static_cast<std::uint16_t>(clearBase + k)), 0);
                }
            }
            if (fi.flatOpcode == FO::InlineReturnVoidJump) {
                as.b(instLabels[fi.trueTargetOffset]);
            }
            break;
        }

        case FO::InlineReturnScalar:
        case FO::InlineReturnScalarJump: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
            }
            if (fi.integerImmediate != 0) {
                const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
                const auto clearBase = static_cast<std::uint16_t>(packed & 0xFFFFu);
                const auto clearCount = static_cast<std::uint32_t>(packed >> 16u);
                as.veorZero128(1);
                for (std::uint32_t k = 0; k < clearCount; ++k) {
                    as.strQ(A64Reg::X20, slotOff(static_cast<std::uint16_t>(clearBase + k)), 1);
                }
            }
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                as.strQ(A64Reg::X20, slotOff(fi.dstSlot), 0);
            }
            if (fi.flatOpcode == FO::InlineReturnScalarJump) {
                as.b(instLabels[fi.trueTargetOffset]);
            }
            break;
        }

        case FO::TermJumpNoArgs:
            emitBudgetDeduct(i32Idx, fi.span, fi.span > 1u);
            as.addImm64(A64Reg::X22, A64Reg::X22, fi.span);
            if (fi.trueTargetOffset != idx + 1u) {
                as.b(instLabels[fi.trueTargetOffset]);
            }
            break;

        case FO::TermBranchBoolNoArgs:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.addImm64(A64Reg::X22, A64Reg::X22, 1);
            as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(fi.src0Slot));
            as.cmpImm32(A64Reg::X0, 0);
            emitCondBranchTargets(A64Cond::NE, i32Idx, fi.trueTargetOffset, fi.falseTargetOffset);
            break;

        case FO::TermBranchGeneric: {
            const auto& cold = execFunc.flatCold[idx];
            if (cold.blockIndex >= execFunc.blocks.size()) return {};
            const auto& blk = execFunc.blocks[cold.blockIndex];
            const auto& term = blk.bytecode->terminator;

            const auto emitEdgeCopies = [&](const runtime::detail::ExecutionEdge& edge) -> bool {
                if (!edge.hasArguments) return true;
                if (!edge.sources || !edge.destinations ||
                    edge.sources->size() != edge.destinations->size() ||
                    edge.sources->size() > execFunc.edgeScratchCount) {
                    return false;
                }
                const auto& srcs = *edge.sources;
                const auto& dsts = *edge.destinations;
                if (srcs.size() == 1u) {
                    const auto sSlot = static_cast<std::uint16_t>(execFunc.registerOffset + srcs[0]);
                    const auto dSlot = static_cast<std::uint16_t>(execFunc.registerOffset + dsts[0].target);
                    if (sSlot != dSlot) {
                        as.ldrQ(0, A64Reg::X20, slotOff(sSlot));
                        as.strQ(A64Reg::X20, slotOff(dSlot), 0);
                    }
                    return true;
                }
                for (std::size_t k = 0; k < srcs.size(); ++k) {
                    const auto sSlot = static_cast<std::uint16_t>(execFunc.registerOffset + srcs[k]);
                    const auto scSlot = static_cast<std::uint16_t>(execFunc.scratchOffset + k);
                    as.ldrQ(0, A64Reg::X20, slotOff(sSlot));
                    as.strQ(A64Reg::X20, slotOff(scSlot), 0);
                }
                for (std::size_t k = 0; k < srcs.size(); ++k) {
                    const auto scSlot = static_cast<std::uint16_t>(execFunc.scratchOffset + k);
                    const auto dSlot = static_cast<std::uint16_t>(execFunc.registerOffset + dsts[k].target);
                    as.ldrQ(0, A64Reg::X20, slotOff(scSlot));
                    as.strQ(A64Reg::X20, slotOff(dSlot), 0);
                }
                return true;
            };

            emitBudgetDeduct(i32Idx, fi.span, false);
            if (term.kind == bytecode::TerminatorKind::Jump) {
                as.addImm64(A64Reg::X22, A64Reg::X22, 1);
                if (!emitEdgeCopies(blk.branch)) return {};
                if (fi.trueTargetOffset != idx + 1u) {
                    as.b(instLabels[fi.trueTargetOffset]);
                }
            } else if (term.kind == bytecode::TerminatorKind::Branch) {
                const auto condSlot = static_cast<std::uint16_t>(execFunc.registerOffset + term.condition);
                if (condSlot >= execFunc.totalFrameSlots || fi.falseTargetOffset >= nInst) {
                    return {};
                }
                emitCheckTagOrTrap(condSlot, kTagBool, i32Idx, 0);
                as.addImm64(A64Reg::X22, A64Reg::X22, 1);
                const std::uint32_t falsePathLbl = as.createLabel();
                as.loadMem8ZeroExt(A64Reg::X0, A64Reg::X20, slotOff(condSlot));
                as.cmpImm32(A64Reg::X0, 0);
                as.bcc(A64Cond::EQ, falsePathLbl);
                if (!emitEdgeCopies(blk.branch)) return {};
                as.b(instLabels[fi.trueTargetOffset]);
                as.bind(falsePathLbl);
                if (!emitEdgeCopies(blk.falseBranch)) return {};
                if (fi.falseTargetOffset != idx + 1u) {
                    as.b(instLabels[fi.falseTargetOffset]);
                }
            } else {
                return {};
            }
            break;
        }

        case FO::TermReturnVoid:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.loadMem64(A64Reg::X0, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.veorZero128(0);
            as.strQ(A64Reg::X0, 0, 0);
            as.b(epilogueOk);
            break;

        case FO::TermReturnScalarFast:
            emitBudgetDeduct(i32Idx, fi.span, false);
            as.ldrQ(0, A64Reg::X20, slotOff(fi.src0Slot));
            as.loadMem64(A64Reg::X0, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.strQ(A64Reg::X0, 0, 0);
            as.b(epilogueOk);
            break;

        case FO::TermReturnGeneric: {
            emitBudgetDeduct(i32Idx, fi.span, false);
            const auto& cold = execFunc.flatCold[idx];
            const auto retReg = execFunc.blocks[cold.blockIndex].bytecode->terminator.value;
            const auto retSlot = static_cast<std::uint16_t>(execFunc.registerOffset + retReg);
            as.ldrQ(0, A64Reg::X20, slotOff(retSlot));
            as.loadMem64(A64Reg::X0, A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, returnSlot)));
            as.strQ(A64Reg::X0, 0, 0);
            as.b(epilogueOk);
            break;
        }

        default:
            return {};
        }
    }

    // Out-of-line Cold Stubs
    for (const auto& stub : coldStubs) {
        as.bind(stub.stubLabel);
        switch (stub.kind) {
        case ColdTrapStub::Kind::BudgetSimple:
            as.storeMem32Imm(
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.b(epilogueBudget);
            break;

        case ColdTrapStub::Kind::BudgetPartialSuper:
            as.addImm64(A64Reg::X21, A64Reg::X21, stub.restoreBudget);
            emitHelperCallInline(&jitPartialSuperBudgetHelper, stub.flatIdx);
            as.b(epilogueBudget);
            break;

        case ColdTrapStub::Kind::IntegerOverflow:
            if (stub.restoreBudget != 0) as.addImm64(A64Reg::X21, A64Reg::X21, stub.restoreBudget);
            as.storeMem32Imm(
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.b(epilogueOverflow);
            break;

        case ColdTrapStub::Kind::DivisionByZero:
            if (stub.restoreBudget != 0) as.addImm64(A64Reg::X21, A64Reg::X21, stub.restoreBudget);
            as.storeMem32Imm(
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.b(epilogueDivZero);
            break;

        case ColdTrapStub::Kind::TypeMismatch:
            if (stub.restoreBudget != 0) as.addImm64(A64Reg::X21, A64Reg::X21, stub.restoreBudget);
            as.storeMem32Imm(
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, errorFlatIndex)),
                stub.flatIdx);
            as.b(epilogueTypeErr);
            break;

        case ColdTrapStub::Kind::ArrayLengthSlow:
            emitHelperCallInline(&jitArrayLengthHelper, stub.flatIdx);
            as.b(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayLengthBranchSlow:
            emitHelperCallInline(&jitArrayLengthHelper, stub.flatIdx);
            as.loadMem32(
                A64Reg::X0,
                A64Reg::X19,
                static_cast<std::int32_t>(offsetof(JitAbiContext, branchCondOut)));
            as.cmpImm32(A64Reg::X0, 0);
            as.bcc(A64Cond::NE, stub.resumeLabel);
            as.b(stub.altResumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayLoadSlow:
            emitHelperCallInline(&jitLoadElementHelper, stub.flatIdx);
            as.b(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::ArrayStoreSlow:
            emitHelperCallInline(&jitStoreElementHelper, stub.flatIdx);
            as.b(stub.resumeLabel);
            break;

        case ColdTrapStub::Kind::FlatStructSlow:
            emitHelperCallInline(&jitFlatStructAndStackRefHelper, stub.flatIdx);
            as.b(stub.resumeLabel);
            break;
        }
    }

    // Shared Exit Epilogues
    as.bind(epilogueOk);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movz32(A64Reg::X0, 0);
    as.b(sharedPopRet);

    as.bind(epilogueBudget);
    as.movz32(A64Reg::X21, 0);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movImm32(A64Reg::X0, static_cast<std::uint32_t>(runtime::ErrorCode::InstructionBudgetExceeded));
    as.storeMem32(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), A64Reg::X0);
    as.b(sharedPopRet);

    as.bind(epilogueOverflow);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movImm32(A64Reg::X0, static_cast<std::uint32_t>(runtime::ErrorCode::IntegerOverflow));
    as.storeMem32(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), A64Reg::X0);
    as.b(sharedPopRet);

    as.bind(epilogueDivZero);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movImm32(A64Reg::X0, static_cast<std::uint32_t>(runtime::ErrorCode::DivisionByZero));
    as.storeMem32(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), A64Reg::X0);
    as.b(sharedPopRet);

    as.bind(epilogueTypeErr);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, remainingBudget)), A64Reg::X21);
    as.storeMem64(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, branchesTaken)), A64Reg::X22);
    as.movImm32(A64Reg::X0, static_cast<std::uint32_t>(runtime::ErrorCode::TypeMismatch));
    as.storeMem32(A64Reg::X19, static_cast<std::int32_t>(offsetof(JitAbiContext, errorCodeRaw)), A64Reg::X0);
    as.b(sharedPopRet);

    as.bind(epilogueHelperErr);

    as.bind(sharedPopRet);
    as.ldp64Sp(A64Reg::X23, A64Reg::X24, 48);
    as.ldp64Sp(A64Reg::X21, A64Reg::X22, 32);
    as.ldp64Sp(A64Reg::X19, A64Reg::X20, 16);
    as.ldp64Sp(A64Reg::X29, A64Reg::X30, 0);
    as.addSp(64);
    as.ret();

    if (!as.ok()) return {};
    return as.bytes();
}

// Exact ARMv8-A A64 user-mode ISA simulator for cross-host verification of emitted ARM64 stencils.
// Executes real 32-bit AArch64 instructions against live JitAbiContext + real C++ BLR helpers,
// while strictly enforcing SP 16-byte alignment, X19..X29 callee-save preservation, and X18 immutability!
[[nodiscard]] std::uint32_t executeSimulatedArm64Code(
    const std::vector<std::uint8_t>& code,
    JitAbiContext* abi,
    std::string& outSimDiagnostic) noexcept {
    if (code.empty() || (code.size() % 4u) != 0u) {
        outSimDiagnostic = "ARM64 code buffer is empty or not 4-byte aligned";
        return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
    }

    struct alignas(16) Vec128 {
        std::uint8_t b[16]{};
    };

    alignas(16) std::uint8_t simStack[4096]{};
    std::uint64_t x[31]{};
    Vec128 v[32]{};
    const auto entrySp = reinterpret_cast<std::uintptr_t>(simStack + sizeof(simStack));
    std::uint64_t sp = static_cast<std::uint64_t>(entrySp);

    constexpr std::uint64_t kReturnSentinel = 0xFFFF0000DEADBEEFULL;
    constexpr std::uint64_t kX18Canary = 0xDEAD1818DEAD1818ULL;
    x[0] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(abi));
    x[18] = kX18Canary;
    for (std::size_t r = 19; r <= 29; ++r) {
        x[r] = 0xA640000000000000ULL | r;
    }
    x[30] = kReturnSentinel;

    bool flagN = false, flagZ = false, flagC = false, flagV = false;

    const auto readX = [&](std::uint32_t r) noexcept -> std::uint64_t {
        return (r == 31u) ? 0ULL : x[r];
    };
    const auto readW = [&](std::uint32_t r) noexcept -> std::uint32_t {
        return (r == 31u) ? 0u : static_cast<std::uint32_t>(x[r]);
    };
    const auto writeX = [&](std::uint32_t r, std::uint64_t val) noexcept {
        if (r < 31u) x[r] = val;
    };
    const auto writeW = [&](std::uint32_t r, std::uint32_t val) noexcept {
        if (r < 31u) x[r] = static_cast<std::uint64_t>(val); // ARM64 32-bit write zero-extends to 64 bits
    };
    const auto readBase = [&](std::uint32_t rn) noexcept -> std::uint64_t {
        return (rn == 31u) ? sp : x[rn];
    };

    const auto evalCond = [&](std::uint32_t cond) noexcept -> bool {
        switch (cond & 0xFu) {
        case 0x0: return flagZ;                          // EQ
        case 0x1: return !flagZ;                         // NE
        case 0x2: return flagC;                          // HS / CS
        case 0x3: return !flagC;                         // LO / CC
        case 0x4: return flagN;                          // MI
        case 0x5: return !flagN;                         // PL
        case 0x6: return flagV;                          // VS
        case 0x7: return !flagV;                         // VC
        case 0x8: return flagC && !flagZ;                // HI
        case 0x9: return !flagC || flagZ;                // LS
        case 0xA: return flagN == flagV;                 // GE
        case 0xB: return flagN != flagV;                 // LT
        case 0xC: return !flagZ && (flagN == flagV);     // GT
        case 0xD: return flagZ || (flagN != flagV);      // LE
        default:  return true;                           // AL
        }
    };

    const auto setFlagsSub32 = [&](std::uint32_t a, std::uint32_t b) noexcept -> std::uint32_t {
        const std::uint32_t res = a - b;
        flagN = (res >> 31u) != 0u;
        flagZ = (res == 0u);
        flagC = (a >= b); // ARM64 SUBS: C=1 when NO borrow!
        flagV = (((a ^ b) & (a ^ res)) >> 31u) != 0u;
        return res;
    };
    const auto setFlagsAdd32 = [&](std::uint32_t a, std::uint32_t b) noexcept -> std::uint32_t {
        const std::uint64_t wide = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
        const auto res = static_cast<std::uint32_t>(wide);
        flagN = (res >> 31u) != 0u;
        flagZ = (res == 0u);
        flagC = (wide >> 32u) != 0u;
        flagV = (((~(a ^ b)) & (a ^ res)) >> 31u) != 0u;
        return res;
    };
    const auto setFlagsSub64 = [&](std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {
        const std::uint64_t res = a - b;
        flagN = (res >> 63u) != 0u;
        flagZ = (res == 0u);
        flagC = (a >= b);
        flagV = (((a ^ b) & (a ^ res)) >> 63u) != 0u;
        return res;
    };
    const auto setFlagsAdd64 = [&](std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {
        const std::uint64_t res = a + b;
        flagN = (res >> 63u) != 0u;
        flagZ = (res == 0u);
        flagC = (res < a);
        flagV = (((~(a ^ b)) & (a ^ res)) >> 63u) != 0u;
        return res;
    };

    std::size_t pc = 0;
    while (pc < code.size()) {
        if (x[18] != kX18Canary) {
            outSimDiagnostic = "ARM64 ABI violation: platform-reserved register X18 was modified";
            return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
        }
        std::uint32_t inst = 0;
        std::memcpy(&inst, code.data() + pc, 4);
        std::size_t nextPc = pc + 4u;

        const std::uint32_t rd = inst & 31u;
        const std::uint32_t rn = (inst >> 5u) & 31u;
        const std::uint32_t rm = (inst >> 16u) & 31u;

        // 1. Unconditional B imm26 (0x14000000 .. 0x17FFFFFF)
        if ((inst & 0xFC000000u) == 0x14000000u) {
            std::int32_t imm26 = static_cast<std::int32_t>(inst & 0x03FFFFFFu);
            if ((imm26 & (1 << 25)) != 0) imm26 |= ~0x03FFFFFF;
            nextPc = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(pc) + static_cast<std::ptrdiff_t>(imm26) * 4);
        }
        // 2. Conditional B.cond imm19 (0x54000000)
        else if ((inst & 0xFF000010u) == 0x54000000u) {
            if (evalCond(inst & 0xFu)) {
                std::int32_t imm19 = static_cast<std::int32_t>((inst >> 5u) & 0x7FFFFu);
                if ((imm19 & (1 << 18)) != 0) imm19 |= ~0x7FFFF;
                nextPc = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(pc) + static_cast<std::ptrdiff_t>(imm19) * 4);
            }
        }
        // 3. RET Xn (0xD65F0000)
        else if ((inst & 0xFFFFFC1Fu) == 0xD65F0000u) {
            const std::uint64_t target = readX(rn);
            if (target != kReturnSentinel) {
                outSimDiagnostic = "ARM64 RET target did not match saved LR";
                return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
            }
            if (sp != static_cast<std::uint64_t>(entrySp)) {
                outSimDiagnostic = "ARM64 SP mismatch on function return";
                return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
            }
            for (std::size_t r = 19; r <= 29; ++r) {
                if (x[r] != (0xA640000000000000ULL | r)) {
                    outSimDiagnostic = "ARM64 AAPCS64 callee-saved register X" + std::to_string(r) + " corrupted";
                    return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
                }
            }
            return static_cast<std::uint32_t>(x[0]);
        }
        // 4. BLR Xn (0xD63F0000) -> call real C++ StencilJit helper!
        else if ((inst & 0xFFFFFC1Fu) == 0xD63F0000u) {
            if ((sp & 0xFULL) != 0ULL) {
                outSimDiagnostic = "ARM64 ABI violation: SP not 16-byte aligned at BLR call site";
                return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
            }
            using HelperFn = std::uint32_t (*)(JitAbiContext*, std::uint32_t);
            const auto calleeAddr = static_cast<std::uintptr_t>(readX(rn));
            const auto helper = bitCastFnPtr<HelperFn>(reinterpret_cast<void*>(calleeAddr));
            auto* ctxArg = reinterpret_cast<JitAbiContext*>(static_cast<std::uintptr_t>(x[0]));
            const auto idxArg = static_cast<std::uint32_t>(x[1]);
            const std::uint32_t ret = helper(ctxArg, idxArg);
            // Poison caller-saved registers (except X0 return & platform-reserved X18) to prove ABI discipline!
            for (std::uint32_t r = 1; r <= 17; ++r) x[r] = 0xCCCCCCCCCCCCCCCCULL;
            for (std::uint32_t vr = 0; vr < 8; ++vr) std::memset(v[vr].b, 0xCC, 16);
            writeW(0, ret);
        }
        // 5. STP / LDP 64-bit signed offset (0xA9000000 / 0xA9400000)
        else if ((inst & 0xFFC00000u) == 0xA9000000u || (inst & 0xFFC00000u) == 0xA9400000u) {
            const bool isLoad = ((inst >> 22u) & 1u) != 0u;
            const std::uint32_t rt2 = (inst >> 10u) & 31u;
            std::int32_t imm7 = static_cast<std::int32_t>((inst >> 15u) & 0x7Fu);
            if ((imm7 & 0x40) != 0) imm7 |= ~0x7F;
            const auto addr = static_cast<std::uintptr_t>(
                static_cast<std::int64_t>(readBase(rn)) + static_cast<std::int64_t>(imm7) * 8LL);
            auto* ptr = reinterpret_cast<std::uint8_t*>(addr);
            if (isLoad) {
                std::uint64_t v1 = 0, v2 = 0;
                std::memcpy(&v1, ptr, 8);
                std::memcpy(&v2, ptr + 8, 8);
                writeX(rd, v1);
                writeX(rt2, v2);
            } else {
                const std::uint64_t v1 = readX(rd);
                const std::uint64_t v2 = readX(rt2);
                std::memcpy(ptr, &v1, 8);
                std::memcpy(ptr + 8, &v2, 8);
            }
        }
        // 6. Move Wide Immediate (MOVN / MOVZ / MOVK, 32-bit & 64-bit)
        else if ((inst & 0x1F800000u) == 0x12800000u) {
            const bool sf64 = (inst >> 31u) != 0u;
            const std::uint32_t opc = (inst >> 29u) & 3u; // 0=MOVN, 2=MOVZ, 3=MOVK
            const std::uint32_t hw = (inst >> 21u) & 3u;
            const std::uint64_t imm16 = (inst >> 5u) & 0xFFFFu;
            const std::uint32_t shift = hw * 16u;
            if (opc == 2u) {
                const std::uint64_t val = imm16 << shift;
                if (sf64) writeX(rd, val); else writeW(rd, static_cast<std::uint32_t>(val));
            } else if (opc == 0u) {
                const std::uint64_t val = ~(imm16 << shift);
                if (sf64) writeX(rd, val); else writeW(rd, static_cast<std::uint32_t>(val));
            } else if (opc == 3u) {
                const std::uint64_t mask = ~(0xFFFFULL << shift);
                if (sf64) {
                    writeX(rd, (readX(rd) & mask) | (imm16 << shift));
                } else {
                    writeW(rd, static_cast<std::uint32_t>((readW(rd) & mask) | (imm16 << shift)));
                }
            }
        }
        // 7. ADD / ADDS / SUB / SUBS (immediate, 32-bit & 64-bit)
        else if ((inst & 0x1F000000u) == 0x11000000u) {
            const bool sf64 = (inst >> 31u) != 0u;
            const bool isSub = ((inst >> 30u) & 1u) != 0u;
            const bool setFlags = ((inst >> 29u) & 1u) != 0u;
            const std::uint32_t sh = (inst >> 22u) & 3u;
            const std::uint64_t imm12 = (inst >> 10u) & 0xFFFu;
            const std::uint64_t imm = (sh == 1u) ? (imm12 << 12u) : imm12;
            const std::uint64_t baseVal = readBase(rn);
            if (sf64) {
                if (setFlags) {
                    const std::uint64_t res = isSub ? setFlagsSub64(baseVal, imm) : setFlagsAdd64(baseVal, imm);
                    writeX(rd, res); // rd==31 -> XZR discard!
                } else {
                    const std::uint64_t res = isSub ? (baseVal - imm) : (baseVal + imm);
                    if (rd == 31u) sp = res; else writeX(rd, res);
                }
            } else {
                const auto b32 = static_cast<std::uint32_t>(baseVal);
                const auto i32 = static_cast<std::uint32_t>(imm);
                if (setFlags) {
                    const std::uint32_t res = isSub ? setFlagsSub32(b32, i32) : setFlagsAdd32(b32, i32);
                    writeW(rd, res);
                } else {
                    const std::uint32_t res = isSub ? (b32 - i32) : (b32 + i32);
                    if (rd == 31u) sp = res; else writeW(rd, res);
                }
            }
        }
        // 8. ADD / ADDS / SUB / SUBS (shifted register, 32-bit & 64-bit)
        else if ((inst & 0x1F200000u) == 0x0B000000u) {
            const bool sf64 = (inst >> 31u) != 0u;
            const bool isSub = ((inst >> 30u) & 1u) != 0u;
            const bool setFlags = ((inst >> 29u) & 1u) != 0u;
            const std::uint32_t shiftAmt = (inst >> 10u) & 63u;
            if (sf64) {
                const std::uint64_t lhs = readX(rn);
                const std::uint64_t rhs = readX(rm) << shiftAmt;
                const std::uint64_t res = setFlags
                    ? (isSub ? setFlagsSub64(lhs, rhs) : setFlagsAdd64(lhs, rhs))
                    : (isSub ? (lhs - rhs) : (lhs + rhs));
                writeX(rd, res);
            } else {
                const std::uint32_t lhs = readW(rn);
                const std::uint32_t rhs = readW(rm) << (shiftAmt & 31u);
                const std::uint32_t res = setFlags
                    ? (isSub ? setFlagsSub32(lhs, rhs) : setFlagsAdd32(lhs, rhs))
                    : (isSub ? (lhs - rhs) : (lhs + rhs));
                writeW(rd, res);
            }
        }
        // 9. Logical (shifted register: ORR / EOR / AND, includes MOV reg)
        else if ((inst & 0x1F200000u) == 0x0A000000u) {
            const bool sf64 = (inst >> 31u) != 0u;
            const std::uint32_t opc = (inst >> 29u) & 3u; // 0=AND, 1=ORR, 2=EOR
            if (sf64) {
                const std::uint64_t lhs = readX(rn);
                const std::uint64_t rhs = readX(rm);
                const std::uint64_t res = (opc == 1u) ? (lhs | rhs) : ((opc == 2u) ? (lhs ^ rhs) : (lhs & rhs));
                writeX(rd, res);
            } else {
                const std::uint32_t lhs = readW(rn);
                const std::uint32_t rhs = readW(rm);
                const std::uint32_t res = (opc == 1u) ? (lhs | rhs) : ((opc == 2u) ? (lhs ^ rhs) : (lhs & rhs));
                writeW(rd, res);
            }
        }
        // 10. Bitfield: SXTW (0x93407C00), UBFM 64-bit LSL (0xD3400000), UBFM 32-bit LSR/UBFX (0x53000000)
        else if ((inst & 0xFFFFFC00u) == 0x93407C00u) {
            writeX(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(readW(rn)))));
        }
        else if ((inst & 0xFFC00000u) == 0xD3400000u) {
            const std::uint32_t immr = (inst >> 16u) & 63u;
            const std::uint32_t imms = (inst >> 10u) & 63u;
            const std::uint64_t src = readX(rn);
            if (imms + 1u == immr) {
                const std::uint32_t lsl = 64u - immr;
                writeX(rd, src << lsl);
            } else if (imms == 63u) {
                writeX(rd, src >> immr);
            } else {
                const std::uint32_t width = imms - immr + 1u;
                writeX(rd, (src >> immr) & ((1ULL << width) - 1ULL));
            }
        }
        else if ((inst & 0xFFC00000u) == 0x53000000u) {
            const std::uint32_t immr = (inst >> 16u) & 31u;
            const std::uint32_t imms = (inst >> 10u) & 31u;
            const std::uint32_t src = readW(rn);
            if (imms == 31u) {
                writeW(rd, src >> immr);
            } else if (immr == 0u) {
                const std::uint32_t mask = (imms == 31u) ? 0xFFFFFFFFu : ((1u << (imms + 1u)) - 1u);
                writeW(rd, src & mask);
            } else {
                const std::uint32_t lsl = 32u - immr;
                writeW(rd, src << lsl);
            }
        }
        // 11. Multiply / Divide: MUL Xd (0x9B007C00), SMULL Xd (0x9B207C00), SDIV Wd (0x1AC00C00), MSUB Wd (0x1B008000)
        else if ((inst & 0xFFE0FC00u) == 0x9B007C00u) {
            writeX(rd, readX(rn) * readX(rm));
        }
        else if ((inst & 0xFFE0FC00u) == 0x9B207C00u) {
            const auto a = static_cast<std::int64_t>(static_cast<std::int32_t>(readW(rn)));
            const auto b = static_cast<std::int64_t>(static_cast<std::int32_t>(readW(rm)));
            writeX(rd, static_cast<std::uint64_t>(a * b));
        }
        else if ((inst & 0xFFE0FC00u) == 0x1AC00C00u) {
            const auto a = static_cast<std::int32_t>(readW(rn));
            const auto b = static_cast<std::int32_t>(readW(rm));
            std::int32_t q = 0;
            if (b != 0) {
                q = (a == std::numeric_limits<std::int32_t>::min() && b == -1) ? a : (a / b);
            }
            writeW(rd, static_cast<std::uint32_t>(q));
        }
        else if ((inst & 0xFFE08000u) == 0x1B008000u) {
            const std::uint32_t ra = (inst >> 10u) & 31u;
            const std::uint32_t res = readW(ra) - (readW(rn) * readW(rm));
            writeW(rd, res);
        }
        // 12. Conditional Select Increment: CSINC Wd, WZR, WZR, invCond (CSET Wd, cond)
        else if ((inst & 0xFFFF0FE0u) == 0x1A9F07E0u) {
            const std::uint32_t invCond = (inst >> 12u) & 0xFu;
            writeW(rd, evalCond(invCond) ? 0u : 1u);
        }
        // 13. Unsigned-Immediate GP & SIMD Loads/Stores
        else if ((inst & 0x3B000000u) == 0x39000000u) {
            const std::uint32_t sizeBits = (inst >> 30u) & 3u;
            const bool isSimd = ((inst >> 26u) & 1u) != 0u;
            const std::uint32_t opc = (inst >> 22u) & 3u;
            const std::uint32_t uimm12 = (inst >> 10u) & 0xFFFu;
            const std::uint64_t baseAddr = readBase(rn);

            if (!isSimd) {
                const std::uint64_t addr = baseAddr + (static_cast<std::uint64_t>(uimm12) << sizeBits);
                auto* ptr = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr));
                if (sizeBits == 0u && opc == 1u) { // LDRB Wt
                    writeW(rd, *ptr);
                } else if (sizeBits == 0u && opc == 0u) { // STRB Wt
                    *ptr = static_cast<std::uint8_t>(readW(rd) & 0xFFu);
                } else if (sizeBits == 1u && opc == 1u) { // LDRH Wt
                    std::uint16_t tmp = 0;
                    std::memcpy(&tmp, ptr, 2);
                    writeW(rd, tmp);
                } else if (sizeBits == 2u && opc == 1u) { // LDR Wt
                    std::uint32_t tmp = 0;
                    std::memcpy(&tmp, ptr, 4);
                    writeW(rd, tmp);
                } else if (sizeBits == 2u && opc == 2u) { // LDRSW Xt
                    std::int32_t tmp = 0;
                    std::memcpy(&tmp, ptr, 4);
                    writeX(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(tmp)));
                } else if (sizeBits == 2u && opc == 0u) { // STR Wt
                    const std::uint32_t tmp = readW(rd);
                    std::memcpy(ptr, &tmp, 4);
                } else if (sizeBits == 3u && opc == 1u) { // LDR Xt
                    std::uint64_t tmp = 0;
                    std::memcpy(&tmp, ptr, 8);
                    writeX(rd, tmp);
                } else if (sizeBits == 3u && opc == 0u) { // STR Xt
                    const std::uint64_t tmp = readX(rd);
                    std::memcpy(ptr, &tmp, 8);
                } else {
                    outSimDiagnostic = "Unsupported GP LDR/STR encoding in ARM64 simulator";
                    return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
                }
            } else {
                if (sizeBits == 0u && (opc == 3u || opc == 2u)) { // LDR Qt (opc=3) / STR Qt (opc=2)
                    const std::uint64_t addr = baseAddr + (static_cast<std::uint64_t>(uimm12) * 16ULL);
                    auto* ptr = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr));
                    if (opc == 3u) std::memcpy(v[rd].b, ptr, 16);
                    else std::memcpy(ptr, v[rd].b, 16);
                } else if (sizeBits == 3u && (opc == 1u || opc == 0u)) { // LDR Dt / STR Dt
                    const std::uint64_t addr = baseAddr + (static_cast<std::uint64_t>(uimm12) * 8ULL);
                    auto* ptr = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr));
                    if (opc == 1u) {
                        std::memset(v[rd].b, 0, 16);
                        std::memcpy(v[rd].b, ptr, 8);
                    } else {
                        std::memcpy(ptr, v[rd].b, 8);
                    }
                } else if (sizeBits == 2u && (opc == 1u || opc == 0u)) { // LDR St / STR St
                    const std::uint64_t addr = baseAddr + (static_cast<std::uint64_t>(uimm12) * 4ULL);
                    auto* ptr = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr));
                    if (opc == 1u) {
                        std::memset(v[rd].b, 0, 16);
                        std::memcpy(v[rd].b, ptr, 4);
                    } else {
                        std::memcpy(ptr, v[rd].b, 4);
                    }
                } else {
                    outSimDiagnostic = "Unsupported SIMD LDR/STR encoding in ARM64 simulator";
                    return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
                }
            }
        }
        // 14. NEON EOR Vd.16B, Vn.16B, Vm.16B (0x6E201C00)
        else if ((inst & 0xFFE0FC00u) == 0x6E201C00u) {
            for (std::size_t k = 0; k < 16u; ++k) {
                v[rd].b[k] = static_cast<std::uint8_t>(v[rn].b[k] ^ v[rm].b[k]);
            }
        }
        // 15. Scalar Double FP: FADD / FSUB / FMUL / FDIV / FCMP
        else if ((inst & 0xFFE0FC00u) == 0x1E602800u ||
                 (inst & 0xFFE0FC00u) == 0x1E603800u ||
                 (inst & 0xFFE0FC00u) == 0x1E600800u ||
                 (inst & 0xFFE0FC00u) == 0x1E601800u) {
            double a = 0.0, b = 0.0;
            std::memcpy(&a, v[rn].b, 8);
            std::memcpy(&b, v[rm].b, 8);
            const std::uint32_t op = (inst >> 12u) & 0xFu;
            double r = 0.0;
            if (op == 2u) r = a + b;
            else if (op == 3u) r = a - b;
            else if (op == 0u) r = a * b;
            else r = a / b;
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &r, 8);
        }
        else if ((inst & 0xFFE0FC1Fu) == 0x1E602000u) {
            double a = 0.0, b = 0.0;
            std::memcpy(&a, v[rn].b, 8);
            std::memcpy(&b, v[rm].b, 8);
            if (std::isnan(a) || std::isnan(b)) {
                flagN = false; flagZ = false; flagC = true; flagV = true;
            } else if (a == b) {
                flagN = false; flagZ = true; flagC = true; flagV = false;
            } else if (a < b) {
                flagN = true; flagZ = false; flagC = false; flagV = false;
            } else {
                flagN = false; flagZ = false; flagC = true; flagV = false;
            }
        }
        // 16. Scalar Float FP: FADD / FSUB / FMUL / FDIV / FCMP
        else if ((inst & 0xFFE0FC00u) == 0x1E202800u ||
                 (inst & 0xFFE0FC00u) == 0x1E203800u ||
                 (inst & 0xFFE0FC00u) == 0x1E200800u ||
                 (inst & 0xFFE0FC00u) == 0x1E201800u) {
            float a = 0.0f, b = 0.0f;
            std::memcpy(&a, v[rn].b, 4);
            std::memcpy(&b, v[rm].b, 4);
            const std::uint32_t op = (inst >> 12u) & 0xFu;
            float r = 0.0f;
            if (op == 2u) r = a + b;
            else if (op == 3u) r = a - b;
            else if (op == 0u) r = a * b;
            else r = a / b;
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &r, 4);
        }
        else if ((inst & 0xFFE0FC1Fu) == 0x1E202000u) {
            float a = 0.0f, b = 0.0f;
            std::memcpy(&a, v[rn].b, 4);
            std::memcpy(&b, v[rm].b, 4);
            if (std::isnan(a) || std::isnan(b)) {
                flagN = false; flagZ = false; flagC = true; flagV = true;
            } else if (a == b) {
                flagN = false; flagZ = true; flagC = true; flagV = false;
            } else if (a < b) {
                flagN = true; flagZ = false; flagC = false; flagV = false;
            } else {
                flagN = false; flagZ = false; flagC = true; flagV = false;
            }
        }
        // 17. FP <-> GP Conversions: FMOV D, X; SCVTF D, X; SCVTF S, X; FCVT D, S; FCVT S, D; FCVTZS X, D
        else if ((inst & 0xFFFFFC00u) == 0x9E670000u) {
            const std::uint64_t bits = readX(rn);
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &bits, 8);
        }
        else if ((inst & 0xFFFFFC00u) == 0x9E620000u) {
            const double dv = static_cast<double>(static_cast<std::int64_t>(readX(rn)));
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &dv, 8);
        }
        else if ((inst & 0xFFFFFC00u) == 0x9E220000u) {
            const float fv = static_cast<float>(static_cast<std::int64_t>(readX(rn)));
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &fv, 4);
        }
        else if ((inst & 0xFFFFFC00u) == 0x1E22C000u) {
            float fv = 0.0f;
            std::memcpy(&fv, v[rn].b, 4);
            const double dv = static_cast<double>(fv);
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &dv, 8);
        }
        else if ((inst & 0xFFFFFC00u) == 0x1E624000u) {
            double dv = 0.0;
            std::memcpy(&dv, v[rn].b, 8);
            const float fv = static_cast<float>(dv);
            std::memset(v[rd].b, 0, 16);
            std::memcpy(v[rd].b, &fv, 4);
        }
        else if ((inst & 0xFFFFFC00u) == 0x9E780000u) {
            double dv = 0.0;
            std::memcpy(&dv, v[rn].b, 8);
            writeX(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(dv)));
        }
        else {
            std::ostringstream oss;
            oss << "Unrecognized ARM64 instruction word 0x" << std::hex << std::setw(8)
                << std::setfill('0') << inst << " at byte offset " << std::dec << pc;
            outSimDiagnostic = oss.str();
            return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
        }

        pc = nextPc;
    }

    outSimDiagnostic = "ARM64 simulator ran off end of code buffer";
    return static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
}

#endif // UINTPTR_MAX == 0xffffffffffffffffULL

// ============================================================================
// P2-A Tier 1: Pre-Bound Direct-Threaded C++ Stencil Thunks (Portable Fallback)
// ============================================================================

[[nodiscard]] runtime::Value& slotAt(JitAbiContext* ctx, std::uint32_t byteOff) noexcept {
    return *reinterpret_cast<runtime::Value*>(
        reinterpret_cast<std::uint8_t*>(ctx->frameBase) + byteOff);
}

[[nodiscard]] StencilStepStatus failThreadedBudget(
    JitAbiContext* ctx,
    const ThreadedStencilOp* op) noexcept {
    if (op->span > 1u) {
        (void)jitPartialSuperBudgetHelper(ctx, op->flatIdx);
        return StencilStepStatus::Error;
    }
    ctx->remainingBudget = 0;
    (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    return StencilStepStatus::Error;
}

[[nodiscard]] bool evalQuickIntStencil(
    JitAbiContext* ctx,
    const ThreadedStencilOp* op,
    runtime::detail::QuickOpcode qop,
    bool checked,
    std::int64_t lhs,
    std::int64_t rhs,
    std::uint8_t unexecAfter,
    runtime::Value& outVal) noexcept {
    using QO = runtime::detail::QuickOpcode;
    switch (qop) {
    case QO::AddInt: {
        const std::int64_t r = lhs + rhs;
        if (checked && !fitsInt32(r)) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::IntegerOverflow, "integer overflow");
            return false;
        }
        outVal.storeInt64(checked ? r : wrapInt32Val(r));
        return true;
    }
    case QO::SubtractInt: {
        const std::int64_t r = lhs - rhs;
        if (checked && !fitsInt32(r)) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::IntegerOverflow, "integer overflow");
            return false;
        }
        outVal.storeInt64(checked ? r : wrapInt32Val(r));
        return true;
    }
    case QO::MultiplyInt: {
        const std::int64_t r = lhs * rhs;
        if (checked && !fitsInt32(r)) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::IntegerOverflow, "integer overflow");
            return false;
        }
        outVal.storeInt64(checked ? r : wrapInt32Val(r));
        return true;
    }
    case QO::DivideInt:
        if (rhs == 0) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::DivisionByZero, "division by zero");
            return false;
        }
        if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::IntegerOverflow, "integer division overflow");
            return false;
        }
        outVal.storeInt64(lhs / rhs);
        return true;
    case QO::RemainderInt:
        if (rhs == 0) {
            ctx->remainingBudget += unexecAfter;
            (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::DivisionByZero, "remainder by zero");
            return false;
        }
        outVal.storeInt64((lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) ? 0 : (lhs % rhs));
        return true;
    case QO::LessInt: outVal.storeBool(lhs < rhs); return true;
    case QO::LessOrEqualInt: outVal.storeBool(lhs <= rhs); return true;
    case QO::GreaterInt: outVal.storeBool(lhs > rhs); return true;
    case QO::GreaterOrEqualInt: outVal.storeBool(lhs >= rhs); return true;
    case QO::EqualInt: outVal.storeBool(lhs == rhs); return true;
    case QO::NotEqualInt: outVal.storeBool(lhs != rhs); return true;
    default:
        (void)setHelperError(ctx, op->flatIdx, runtime::ErrorCode::InvalidProgram, "invalid quick int op");
        return false;
    }
}

StencilStepStatus th_MoveSlot(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    slotAt(ctx, pc->dstOff) = slotAt(ctx, pc->src0Off);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_SuperMoveParamStore(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    const runtime::Value v = slotAt(ctx, pc->src0Off);
    if (pc->auxByte != 0u) slotAt(ctx, pc->dstOff) = v;
    slotAt(ctx, pc->storeOff) = v;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstInt(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    slotAt(ctx, pc->dstOff).storeInt64(pc->imm64);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstLong(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    slotAt(ctx, pc->dstOff).storeLong(pc->imm64);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstBool(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    slotAt(ctx, pc->dstOff).storeBool(pc->imm64 != 0);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstDouble(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    double d = 0.0;
    std::memcpy(&d, &pc->imm64, sizeof(double));
    slotAt(ctx, pc->dstOff).storeDouble(d);
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ConstFloat(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    float f = 0.0f;
    std::memcpy(&f, &pc->imm64, sizeof(float));
    slotAt(ctx, pc->auxByte != 0u ? pc->storeOff : pc->dstOff).storeFloat(f);
    ++pc;
    return StencilStepStatus::Continue;
}

template <runtime::detail::QuickOpcode Op, bool Checked, bool Trusted>
StencilStepStatus th_BinaryInt(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    std::int64_t a = 0, b = 0;
    if constexpr (Trusted) {
        a = slotAt(ctx, pc->src0Off).asInt64Unchecked();
        b = slotAt(ctx, pc->src1Off).asInt64Unchecked();
    } else {
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(a) || !slotAt(ctx, pc->src1Off).tryGetInt64(b)) {
            ctx->remainingBudget += pc->unexecutedOnTrap;
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
            return StencilStepStatus::Error;
        }
    }
    if (!evalQuickIntStencil(ctx, pc, Op, Checked, a, b, pc->unexecutedOnTrap, slotAt(ctx, pc->dstOff))) {
        return StencilStepStatus::Error;
    }
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_SuperBinaryIntGeneric(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    using FO = runtime::detail::FlatOpcode;

    std::int64_t lhs = 0;
    if (!slotAt(ctx, pc->src0Off).tryGetInt64(lhs)) {
        ctx->remainingBudget += pc->unexecutedOnTrap;
        (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
        return StencilStepStatus::Error;
    }

    if (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStore ||
        fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStoreElided) {
        std::int64_t rhs1 = 0, rhs2 = 0;
        if (!slotAt(ctx, pc->src1Off).tryGetInt64(rhs1)) {
            ctx->remainingBudget += 2u;
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
            return StencilStepStatus::Error;
        }
        if (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStore) {
            slotAt(ctx, pc->dstOff).storeInt64(lhs);
        }
        runtime::Value mid{};
        if (!evalQuickIntStencil(ctx, pc, fi.binaryOpcode1, fi.checked1(), lhs, rhs1, 2u, mid)) {
            return StencilStepStatus::Error;
        }
        if (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStore) {
            slotAt(ctx, pc->aux0Off) = mid;
        }
        if (!slotAt(ctx, pc->src2Off).tryGetInt64(rhs2)) {
            ctx->remainingBudget += 1u;
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
            return StencilStepStatus::Error;
        }
        runtime::Value out{};
        if (!evalQuickIntStencil(ctx, pc, fi.binaryOpcode2, fi.checked2(), mid.asInt64Unchecked(), rhs2, 1u, out)) {
            return StencilStepStatus::Error;
        }
        if (fi.flatOpcode == FO::SuperBinaryLocalIntRegChainStore) {
            slotAt(ctx, pc->aux1Off) = out;
        }
        slotAt(ctx, pc->storeOff) = out;
        ++pc;
        return StencilStepStatus::Continue;
    }

    const bool isConst =
        fi.flatOpcode == FO::SuperBinaryLocalIntConst ||
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStore ||
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreElided ||
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump;
    std::int64_t rhs = pc->imm64;
    if (!isConst && !slotAt(ctx, pc->src1Off).tryGetInt64(rhs)) {
        ctx->remainingBudget += pc->unexecutedOnTrap;
        (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int operand");
        return StencilStepStatus::Error;
    }

    const bool isJump =
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump ||
        fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump ||
        fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreJump;
    const bool isElided =
        fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreElided ||
        fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreElided ||
        fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreElided;

    if (!isElided && !isJump) {
        slotAt(ctx, pc->dstOff).storeInt64(lhs);
        if (isConst || fi.flatOpcode == FO::SuperBinaryLocalIntLocal || fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore) {
            slotAt(ctx, pc->aux0Off).storeInt64(rhs);
        }
    }

    runtime::Value out{};
    if (!evalQuickIntStencil(ctx, pc, fi.binaryOpcode1, fi.checked1(), lhs, rhs, pc->unexecutedOnTrap, out)) {
        return StencilStepStatus::Error;
    }
    if (isJump) {
        slotAt(ctx, pc->dstOff) = out;
        ctx->branchesTaken += ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) ? 2u : 1u;
        pc = pc->trueTarget;
        return StencilStepStatus::Continue;
    }
    if (isElided) {
        slotAt(ctx, pc->storeOff) = out;
    } else {
        slotAt(ctx, pc->aux1Off) = out;
        if (fi.flatOpcode == FO::SuperBinaryLocalIntConstStore ||
            fi.flatOpcode == FO::SuperBinaryLocalIntLocalStore ||
            fi.flatOpcode == FO::SuperBinaryLocalIntRegStore) {
            slotAt(ctx, pc->storeOff) = out;
        }
    }
    ++pc;
    return StencilStepStatus::Continue;
}

template <runtime::detail::QuickOpcode CmpOp, bool IsConst>
StencilStepStatus th_SuperLessBranch(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    std::int64_t lhs = 0, rhs = pc->imm64;
    if (!slotAt(ctx, pc->src0Off).tryGetInt64(lhs) ||
        (!IsConst && !slotAt(ctx, pc->src1Off).tryGetInt64(rhs))) {
        ctx->remainingBudget -= (pc->span - 1u);
        (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "expected int in branch");
        return StencilStepStatus::Error;
    }
    ctx->remainingBudget -= pc->span;
    ++ctx->branchesTaken;
    bool cond = false;
    using QO = runtime::detail::QuickOpcode;
    if constexpr (CmpOp == QO::LessInt) cond = (lhs < rhs);
    else if constexpr (CmpOp == QO::LessOrEqualInt) cond = (lhs <= rhs);
    else if constexpr (CmpOp == QO::GreaterInt) cond = (lhs > rhs);
    else if constexpr (CmpOp == QO::GreaterOrEqualInt) cond = (lhs >= rhs);
    else if constexpr (CmpOp == QO::EqualInt) cond = (lhs == rhs);
    else cond = (lhs != rhs);
    pc = cond ? pc->trueTarget : pc->falseTarget;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ArrayLengthBranch(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitArrayLengthHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    pc = (ctx->branchCondOut != 0u) ? pc->trueTarget : pc->falseTarget;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ArrayLength(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitArrayLengthHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_NewArray(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitNewArrayHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_FlatStructOrStackRef(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitFlatStructAndStackRefHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_LoadElement(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    // Fast inline packed int32 / double hit path before calling helper
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
    const auto& arrVal = slotAt(ctx, pc->src0Off);
    if (arrVal.rawTag() == runtime::ValueTag::ObjectRef && ce.isPacked != 0u) {
        std::uint64_t sg = 0;
        std::memcpy(&sg, &arrVal, 8);
        if (sg == ce.slotAndGen && arrVal.u_.objectRef.heapId == ce.heapId) {
            std::int64_t idx = 0;
            if (slotAt(ctx, pc->src1Off).tryGetInt64(idx) &&
                static_cast<std::uint64_t>(idx) < ce.length) {
                if (ce.encoding == kPackedInt32 &&
                    (fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementIntFast ||
                     (static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1)) == semantic::PrimitiveType::Int &&
                      fi.integerImmediate == 0))) {
                    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
                    ctx->remainingBudget -= pc->span;
                    slotAt(ctx, pc->dstOff).storeInt64(
                        static_cast<std::int64_t>(static_cast<const std::int32_t*>(ce.rawData)[idx]));
                    ++pc;
                    return StencilStepStatus::Continue;
                }
                if (ce.encoding == kPackedFloat64 &&
                    fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementTypedFast &&
                    static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1)) == semantic::PrimitiveType::Double) {
                    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
                    ctx->remainingBudget -= pc->span;
                    slotAt(ctx, pc->dstOff).storeDouble(static_cast<const double*>(ce.rawData)[idx]);
                    ++pc;
                    return StencilStepStatus::Continue;
                }
            }
        }
    }
    if (jitLoadElementHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_StoreElement(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
    const auto& arrVal = slotAt(ctx, pc->src0Off);
    if (arrVal.rawTag() == runtime::ValueTag::ObjectRef && ce.isPacked != 0u) {
        std::uint64_t sg = 0;
        std::memcpy(&sg, &arrVal, 8);
        if (sg == ce.slotAndGen && arrVal.u_.objectRef.heapId == ce.heapId) {
            std::int64_t idx = 0;
            if (slotAt(ctx, pc->src1Off).tryGetInt64(idx) &&
                static_cast<std::uint64_t>(idx) < ce.length) {
                const auto& val = slotAt(ctx, pc->src2Off);
                std::int64_t iv = 0;
                if (ce.encoding == kPackedInt32 && val.tryGetInt64(iv) && fitsInt32(iv)) {
                    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
                    ctx->remainingBudget -= pc->span;
                    static_cast<std::int32_t*>(ce.rawData)[idx] = static_cast<std::int32_t>(iv);
                    ++pc;
                    return StencilStepStatus::Continue;
                }
                double dv = 0.0;
                if (ce.encoding == kPackedFloat64 && val.tryGetDouble(dv)) {
                    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
                    ctx->remainingBudget -= pc->span;
                    static_cast<double*>(ce.rawData)[idx] = dv;
                    ++pc;
                    return StencilStepStatus::Continue;
                }
            }
        }
    }
    if (jitStoreElementHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ScalarMisc(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    using FO = runtime::detail::FlatOpcode;
    using QO = runtime::detail::QuickOpcode;

    const auto failType = [&]() noexcept {
        ctx->remainingBudget -= (pc->span - pc->unexecutedOnTrap);
        (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "scalar type mismatch");
        return StencilStepStatus::Error;
    };

    switch (fi.flatOpcode) {
    case FO::NegateIntUnchecked: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeInt64(v == std::numeric_limits<std::int32_t>::min() ? v : -v);
        break;
    }
    case FO::LogicalNot: {
        bool b = false;
        if (!slotAt(ctx, pc->src0Off).tryGetBool(b)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeBool(!b);
        break;
    }
    case FO::AddDouble: case FO::SubDouble: case FO::MulDouble: case FO::DivDouble:
    case FO::LessDouble: case FO::LessOrEqualDouble: case FO::GreaterDouble: case FO::GreaterOrEqualDouble:
    case FO::AddDoubleTrusted: case FO::SubDoubleTrusted: case FO::MulDoubleTrusted: case FO::DivDoubleTrusted: {
        double a = 0.0, b = 0.0;
        if (!slotAt(ctx, pc->src0Off).tryGetDouble(a) || !slotAt(ctx, pc->src1Off).tryGetDouble(b)) return failType();
        ctx->remainingBudget -= pc->span;
        switch (fi.flatOpcode) {
        case FO::AddDouble: case FO::AddDoubleTrusted: slotAt(ctx, pc->dstOff).storeDouble(a + b); break;
        case FO::SubDouble: case FO::SubDoubleTrusted: slotAt(ctx, pc->dstOff).storeDouble(a - b); break;
        case FO::MulDouble: case FO::MulDoubleTrusted: slotAt(ctx, pc->dstOff).storeDouble(a * b); break;
        case FO::DivDouble: case FO::DivDoubleTrusted: slotAt(ctx, pc->dstOff).storeDouble(a / b); break;
        case FO::LessDouble: slotAt(ctx, pc->dstOff).storeBool(a < b); break;
        case FO::LessOrEqualDouble: slotAt(ctx, pc->dstOff).storeBool(a <= b); break;
        case FO::GreaterDouble: slotAt(ctx, pc->dstOff).storeBool(a > b); break;
        default: slotAt(ctx, pc->dstOff).storeBool(a >= b); break;
        }
        break;
    }
    case FO::NegateDouble: {
        double d = 0.0;
        if (!slotAt(ctx, pc->src0Off).tryGetDouble(d)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeDouble(-d);
        break;
    }
    case FO::SuperBinaryLocalDoubleConstStore:
    case FO::SuperBinaryLocalDoubleConstStoreElided:
    case FO::SuperBinaryLocalDoubleLocalStore:
    case FO::SuperBinaryLocalDoubleLocalStoreElided: {
        double a = 0.0, b = 0.0;
        if (!slotAt(ctx, pc->src0Off).tryGetDouble(a)) return failType();
        const bool isConst = (fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStore ||
                              fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStoreElided);
        if (isConst) std::memcpy(&b, &pc->imm64, 8);
        else if (!slotAt(ctx, pc->src1Off).tryGetDouble(b)) return failType();
        double out = 0.0;
        if (fi.binaryOpcode1 == QO::AddDouble) out = a + b;
        else if (fi.binaryOpcode1 == QO::SubtractDouble) out = a - b;
        else if (fi.binaryOpcode1 == QO::MultiplyDouble) out = a * b;
        else out = a / b;
        ctx->remainingBudget -= pc->span;
        if (fi.flatOpcode == FO::SuperBinaryLocalDoubleConstStore ||
            fi.flatOpcode == FO::SuperBinaryLocalDoubleLocalStore) {
            slotAt(ctx, pc->dstOff).storeDouble(a);
            slotAt(ctx, pc->aux0Off).storeDouble(b);
            slotAt(ctx, pc->aux1Off).storeDouble(out);
        }
        slotAt(ctx, pc->storeOff).storeDouble(out);
        break;
    }
    case FO::AddFloat: case FO::SubFloat: case FO::MulFloat: case FO::DivFloat:
    case FO::LessFloat: case FO::LessOrEqualFloat: case FO::GreaterFloat: case FO::GreaterOrEqualFloat:
    case FO::EqualFloat: case FO::NotEqualFloat:
    case FO::AddFloatTrusted: case FO::SubFloatTrusted: case FO::MulFloatTrusted: {
        float a = 0.0f, b = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(a) || !slotAt(ctx, pc->src1Off).tryGetFloat(b)) return failType();
        ctx->remainingBudget -= pc->span;
        switch (fi.flatOpcode) {
        case FO::AddFloat: case FO::AddFloatTrusted: slotAt(ctx, pc->dstOff).storeFloat(a + b); break;
        case FO::SubFloat: case FO::SubFloatTrusted: slotAt(ctx, pc->dstOff).storeFloat(a - b); break;
        case FO::MulFloat: case FO::MulFloatTrusted: slotAt(ctx, pc->dstOff).storeFloat(a * b); break;
        case FO::DivFloat: slotAt(ctx, pc->dstOff).storeFloat(a / b); break;
        case FO::LessFloat: slotAt(ctx, pc->dstOff).storeBool(a < b); break;
        case FO::LessOrEqualFloat: slotAt(ctx, pc->dstOff).storeBool(a <= b); break;
        case FO::GreaterFloat: slotAt(ctx, pc->dstOff).storeBool(a > b); break;
        case FO::GreaterOrEqualFloat: slotAt(ctx, pc->dstOff).storeBool(a >= b); break;
        case FO::EqualFloat: slotAt(ctx, pc->dstOff).storeBool(a == b); break;
        default: slotAt(ctx, pc->dstOff).storeBool(a != b); break;
        }
        break;
    }
    case FO::NegateFloat: {
        float f = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(f)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeFloat(-f);
        break;
    }
    case FO::SuperBinaryLocalFloatLocalStore:
    case FO::SuperBinaryLocalFloatLocalStoreElided:
    case FO::SuperBinaryLocalFloatReg:
    case FO::SuperBinaryLocalFloatRegElided:
    case FO::SuperBinaryLocalFloatRegStore:
    case FO::SuperBinaryLocalFloatRegStoreElided:
    case FO::SuperBinaryRegFloatStoreElided: {
        float a = 0.0f, b = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(a) || !slotAt(ctx, pc->src1Off).tryGetFloat(b)) return failType();
        float out = 0.0f;
        if (fi.binaryOpcode1 == QO::AddFloat) out = a + b;
        else if (fi.binaryOpcode1 == QO::SubtractFloat) out = a - b;
        else if (fi.binaryOpcode1 == QO::MultiplyFloat) out = a * b;
        else out = a / b;
        ctx->remainingBudget -= pc->span;
        if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
            slotAt(ctx, pc->dstOff).storeFloat(a);
            if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore) slotAt(ctx, pc->aux0Off).storeFloat(b);
        }
        if (fi.flatOpcode == FO::SuperBinaryLocalFloatLocalStore ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatReg ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatRegElided ||
            fi.flatOpcode == FO::SuperBinaryLocalFloatRegStore) {
            slotAt(ctx, pc->aux1Off).storeFloat(out);
        }
        if (fi.flatOpcode != FO::SuperBinaryLocalFloatReg && fi.flatOpcode != FO::SuperBinaryLocalFloatRegElided) {
            slotAt(ctx, pc->storeOff).storeFloat(out);
        }
        break;
    }
    case FO::ConvertIntToLongFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeLong(v);
        break;
    }
    case FO::ConvertIntToDoubleFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeDouble(static_cast<double>(v));
        break;
    }
    case FO::ConvertLongToDoubleFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetLong(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeDouble(static_cast<double>(v));
        break;
    }
    case FO::ConvertIntToFloatFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetInt64(v)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeFloat(static_cast<float>(v));
        break;
    }
    case FO::ConvertFloatToDoubleFast: {
        float f = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(f)) return failType();
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeDouble(static_cast<double>(f));
        break;
    }
    case FO::ConvertDoubleToFloatFast: {
        double d = 0.0;
        if (!slotAt(ctx, pc->src0Off).tryGetDouble(d)) return failType();
        if (fi.checked1() &&
            !(d >= -static_cast<double>(std::numeric_limits<float>::max()) &&
              d <= static_cast<double>(std::numeric_limits<float>::max()))) {
            ctx->remainingBudget -= (pc->span - pc->unexecutedOnTrap);
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::IntegerOverflow, "numeric conversion overflow");
            return StencilStepStatus::Error;
        }
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeFloat(static_cast<float>(d));
        break;
    }
    case FO::ConvertFloatToIntFast: {
        float f = 0.0f;
        if (!slotAt(ctx, pc->src0Off).tryGetFloat(f)) return failType();
        const double d = static_cast<double>(f);
        if (!(d >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
              d <= static_cast<double>(std::numeric_limits<std::int32_t>::max()))) {
            ctx->remainingBudget -= (pc->span - pc->unexecutedOnTrap);
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::IntegerOverflow, "numeric conversion overflow");
            return StencilStepStatus::Error;
        }
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeInt64(static_cast<std::int64_t>(d));
        break;
    }
    case FO::ConvertLongToIntFast: {
        std::int64_t v = 0;
        if (!slotAt(ctx, pc->src0Off).tryGetLong(v)) return failType();
        if (fi.checked1() && !fitsInt32(v)) {
            ctx->remainingBudget -= (pc->span - pc->unexecutedOnTrap);
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::IntegerOverflow, "numeric conversion overflow");
            return StencilStepStatus::Error;
        }
        ctx->remainingBudget -= pc->span;
        slotAt(ctx, pc->dstOff).storeInt64(fi.checked1() ? v : wrapInt32Val(v));
        break;
    }
    default:
        return failType();
    }
    ++pc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_InlineCallEnter(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (jitInlineCallEnterHelper(ctx, pc->flatIdx) != 0u) return StencilStepStatus::Error;
    if (ctx->nextFlatOffsetOut != 0u) {
        pc += (1u + pc->auxByte);
    } else {
        ++pc;
    }
    return StencilStepStatus::Continue;
}

StencilStepStatus th_InlineReturn(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    const auto& fi = ctx->compiledFn->execFunc->flatStream[pc->flatIdx];
    using FO = runtime::detail::FlatOpcode;
    runtime::Value retVal{};
    const bool hasScalar =
        (fi.flatOpcode == FO::InlineReturnScalar || fi.flatOpcode == FO::InlineReturnScalarJump) &&
        fi.dstSlot != runtime::detail::InvalidCompactReg;
    if (hasScalar) retVal = slotAt(ctx, pc->src0Off);
    if (pc->imm64 != 0) {
        const auto packed = static_cast<std::uint64_t>(pc->imm64);
        std::memset(
            static_cast<void*>(ctx->frameBase + (packed & 0xFFFFu)),
            0,
            static_cast<std::size_t>(packed >> 16u) * sizeof(runtime::Value));
    }
    if (hasScalar) slotAt(ctx, pc->dstOff) = retVal;
    if (fi.flatOpcode == FO::InlineReturnVoidJump || fi.flatOpcode == FO::InlineReturnScalarJump) {
        pc = pc->trueTarget;
    } else {
        ++pc;
    }
    return StencilStepStatus::Continue;
}

StencilStepStatus th_Jump(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    ctx->branchesTaken += pc->span;
    pc = pc->trueTarget;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_BranchBool(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    ++ctx->branchesTaken;
    pc = slotAt(ctx, pc->src0Off).asBoolUnchecked() ? pc->trueTarget : pc->falseTarget;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_BranchGeneric(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& cold = execFunc.flatCold[pc->flatIdx];
    const auto& blk = execFunc.blocks[cold.blockIndex];
    const auto& term = blk.bytecode->terminator;
    const runtime::detail::ExecutionEdge* edge = &blk.branch;
    const ThreadedStencilOp* nextPc = pc->trueTarget;
    if (term.kind == bytecode::TerminatorKind::Branch) {
        const std::size_t condSlot = execFunc.registerOffset + term.condition;
        bool b = false;
        if (condSlot >= execFunc.totalFrameSlots || !ctx->frameBase[condSlot].tryGetBool(b)) {
            (void)setHelperError(ctx, pc->flatIdx, runtime::ErrorCode::TypeMismatch, "branch condition expected bool");
            return StencilStepStatus::Error;
        }
        if (!b) {
            edge = &blk.falseBranch;
            nextPc = pc->falseTarget;
        }
    }
    ++ctx->branchesTaken;
    if (edge->hasArguments && edge->sources && edge->destinations) {
        const auto& srcs = *edge->sources;
        const auto& dsts = *edge->destinations;
        runtime::Value* const regPtr = ctx->frameBase + execFunc.registerOffset;
        runtime::Value* const scratchPtr = ctx->frameBase + execFunc.scratchOffset;
        for (std::size_t k = 0; k < srcs.size(); ++k) {
            scratchPtr[k] = regPtr[srcs[k]];
        }
        for (std::size_t k = 0; k < srcs.size(); ++k) {
            regPtr[dsts[k].target] = scratchPtr[k];
        }
    }
    pc = nextPc;
    return StencilStepStatus::Continue;
}

StencilStepStatus th_ReturnVoid(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    *ctx->returnSlot = runtime::Value{};
    return StencilStepStatus::ReturnOk;
}

StencilStepStatus th_ReturnValue(JitAbiContext* ctx, const ThreadedStencilOp*& pc) noexcept {
    if (ctx->remainingBudget < pc->span) return failThreadedBudget(ctx, pc);
    ctx->remainingBudget -= pc->span;
    *ctx->returnSlot = slotAt(ctx, pc->src0Off);
    return StencilStepStatus::ReturnOk;
}

void buildThreadedStream(CompiledFunctionRecord& rec) {
    const auto& execFunc = *rec.execFunc;
    const std::size_t n = execFunc.flatStream.size();
    rec.threadedStream.resize(n);

    const auto toOff = [](std::uint16_t slot) noexcept -> std::uint32_t {
        return slot == runtime::detail::InvalidCompactReg
            ? 0u
            : static_cast<std::uint32_t>(slot) * 16u;
    };

    for (std::size_t i = 0; i < n; ++i) {
        const auto& fi = execFunc.flatStream[i];
        auto& op = rec.threadedStream[i];
        op.imm64 = fi.integerImmediate;
        op.dstOff = toOff(fi.dstSlot);
        op.src0Off = toOff(fi.src0Slot);
        op.src1Off = toOff(fi.src1Slot);
        op.src2Off = toOff(fi.src2Slot);
        op.aux0Off = toOff(fi.aux0Slot);
        op.aux1Off = toOff(fi.aux1Slot);
        op.storeOff = toOff(fi.storeSlot);
        op.flatIdx = static_cast<std::uint32_t>(i);
        op.span = fi.span;
        op.unexecutedOnTrap = rec.unexecutedAfterOp[i];
        op.flags = fi.arithFlags;

        using FO = runtime::detail::FlatOpcode;
        using QO = runtime::detail::QuickOpcode;
        switch (fi.flatOpcode) {
        case FO::MoveSlot:
        case FO::AliasStructSlice:
            op.handler = &th_MoveSlot;
            break;
        case FO::InitStructSlice:
        case FO::CopyStructSlice:
        case FO::LoadStructSliceField:
        case FO::StoreStructSliceField:
        case FO::StoreLocalStructFieldInPlace:
        case FO::InitStackRefCell:
        case FO::BindLocalStackRef:
        case FO::WritebackLocalStackRef:
        case FO::LoadStackRefCell:
        case FO::CheckNotNullFast:
        case FO::StoreStackRefCell:
        case FO::StoreStackRefStructFieldInPlace:
        case FO::NewObjectFast:
        case FO::LoadFieldExactFast:
        case FO::LoadFieldFast:
        case FO::StoreFieldExactFast:
        case FO::StoreFieldFast:
        case FO::GenericBytecode:
            op.handler = &th_FlatStructOrStackRef;
            break;
        case FO::SuperMoveParamStoreLocal:
            op.auxByte = 1u;
            op.handler = &th_SuperMoveParamStore;
            break;
        case FO::SuperMoveParamStoreLocalElided:
            op.auxByte = 0u;
            op.handler = &th_SuperMoveParamStore;
            break;
        case FO::ConstantInt: op.handler = &th_ConstInt; break;
        case FO::ConstantLong: op.handler = &th_ConstLong; break;
        case FO::ConstantBool: op.handler = &th_ConstBool; break;
        case FO::ConstantDouble: op.handler = &th_ConstDouble; break;
        case FO::SuperConstFloat:
            op.auxByte = 0u;
            op.handler = &th_ConstFloat;
            break;
        case FO::SuperConstFloatStoreLocal:
            op.auxByte = 1u;
            op.handler = &th_ConstFloat;
            break;
        case FO::AddIntChecked: op.handler = &th_BinaryInt<QO::AddInt, true, false>; break;
        case FO::AddIntUnchecked: op.handler = &th_BinaryInt<QO::AddInt, false, false>; break;
        case FO::SubIntChecked: op.handler = &th_BinaryInt<QO::SubtractInt, true, false>; break;
        case FO::SubIntUnchecked: op.handler = &th_BinaryInt<QO::SubtractInt, false, false>; break;
        case FO::MulIntChecked: op.handler = &th_BinaryInt<QO::MultiplyInt, true, false>; break;
        case FO::MulIntUnchecked: op.handler = &th_BinaryInt<QO::MultiplyInt, false, false>; break;
        case FO::DivInt: op.handler = &th_BinaryInt<QO::DivideInt, true, false>; break;
        case FO::RemInt: op.handler = &th_BinaryInt<QO::RemainderInt, true, false>; break;
        case FO::LessInt: op.handler = &th_BinaryInt<QO::LessInt, false, false>; break;
        case FO::LessOrEqualInt: op.handler = &th_BinaryInt<QO::LessOrEqualInt, false, false>; break;
        case FO::GreaterInt: op.handler = &th_BinaryInt<QO::GreaterInt, false, false>; break;
        case FO::GreaterOrEqualInt: op.handler = &th_BinaryInt<QO::GreaterOrEqualInt, false, false>; break;
        case FO::EqualInt: op.handler = &th_BinaryInt<QO::EqualInt, false, false>; break;
        case FO::NotEqualInt: op.handler = &th_BinaryInt<QO::NotEqualInt, false, false>; break;
        case FO::AddIntTrustedChecked: op.handler = &th_BinaryInt<QO::AddInt, true, true>; break;
        case FO::AddIntTrustedUnchecked: op.handler = &th_BinaryInt<QO::AddInt, false, true>; break;
        case FO::SubIntTrustedChecked: op.handler = &th_BinaryInt<QO::SubtractInt, true, true>; break;
        case FO::SubIntTrustedUnchecked: op.handler = &th_BinaryInt<QO::SubtractInt, false, true>; break;
        case FO::MulIntTrustedChecked: op.handler = &th_BinaryInt<QO::MultiplyInt, true, true>; break;
        case FO::MulIntTrustedUnchecked: op.handler = &th_BinaryInt<QO::MultiplyInt, false, true>; break;
        case FO::LessIntTrusted: op.handler = &th_BinaryInt<QO::LessInt, false, true>; break;

        case FO::SuperBinaryLocalIntConst:
        case FO::SuperBinaryLocalIntConstStore:
        case FO::SuperBinaryLocalIntConstStoreElided:
        case FO::SuperBinaryLocalIntConstStoreJump:
        case FO::SuperBinaryLocalIntLocal:
        case FO::SuperBinaryLocalIntLocalStore:
        case FO::SuperBinaryLocalIntLocalStoreElided:
        case FO::SuperBinaryLocalIntLocalStoreJump:
        case FO::SuperBinaryLocalIntReg:
        case FO::SuperBinaryLocalIntRegStore:
        case FO::SuperBinaryLocalIntRegStoreElided:
        case FO::SuperBinaryLocalIntRegStoreJump:
        case FO::SuperBinaryLocalIntRegChainStore:
        case FO::SuperBinaryLocalIntRegChainStoreElided:
            op.handler = &th_SuperBinaryIntGeneric;
            if (fi.flatOpcode == FO::SuperBinaryLocalIntConstStoreJump ||
                fi.flatOpcode == FO::SuperBinaryLocalIntLocalStoreJump ||
                fi.flatOpcode == FO::SuperBinaryLocalIntRegStoreJump) {
                op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            }
            break;

        case FO::SuperLessLocalConstBranch:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = &rec.threadedStream[fi.falseTargetOffset];
            switch (fi.binaryOpcode1) {
            case QO::LessInt: op.handler = &th_SuperLessBranch<QO::LessInt, true>; break;
            case QO::LessOrEqualInt: op.handler = &th_SuperLessBranch<QO::LessOrEqualInt, true>; break;
            case QO::GreaterInt: op.handler = &th_SuperLessBranch<QO::GreaterInt, true>; break;
            case QO::GreaterOrEqualInt: op.handler = &th_SuperLessBranch<QO::GreaterOrEqualInt, true>; break;
            case QO::EqualInt: op.handler = &th_SuperLessBranch<QO::EqualInt, true>; break;
            default: op.handler = &th_SuperLessBranch<QO::NotEqualInt, true>; break;
            }
            break;

        case FO::SuperLessLocalRegBranch:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = &rec.threadedStream[fi.falseTargetOffset];
            switch (fi.binaryOpcode1) {
            case QO::LessInt: op.handler = &th_SuperLessBranch<QO::LessInt, false>; break;
            case QO::LessOrEqualInt: op.handler = &th_SuperLessBranch<QO::LessOrEqualInt, false>; break;
            case QO::GreaterInt: op.handler = &th_SuperLessBranch<QO::GreaterInt, false>; break;
            case QO::GreaterOrEqualInt: op.handler = &th_SuperLessBranch<QO::GreaterOrEqualInt, false>; break;
            case QO::EqualInt: op.handler = &th_SuperLessBranch<QO::EqualInt, false>; break;
            default: op.handler = &th_SuperLessBranch<QO::NotEqualInt, false>; break;
            }
            break;

        case FO::SuperLessLocalArrayLengthBranch:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = &rec.threadedStream[fi.falseTargetOffset];
            op.handler = &th_ArrayLengthBranch;
            break;

        case FO::ArrayLengthFast: op.handler = &th_ArrayLength; break;
        case FO::NewArrayFast: op.handler = &th_NewArray; break;
        case FO::LoadElementIntFast:
        case FO::LoadElementTypedFast:
            op.handler = &th_LoadElement;
            break;
        case FO::StoreElementIntFast:
        case FO::StoreElementPrimFast:
        case FO::StoreElementStructFast:
        case FO::StoreElementObjFast:
        case FO::StoreElementStructFieldInPlace:
            op.handler = &th_StoreElement;
            break;

        case FO::InlineCallEnter:
            op.auxByte = fi.inlineArgCount;
            op.handler = &th_InlineCallEnter;
            break;
        case FO::InlineReturnVoid:
        case FO::InlineReturnVoidJump:
        case FO::InlineReturnScalar:
        case FO::InlineReturnScalarJump:
            if (fi.flatOpcode == FO::InlineReturnVoidJump ||
                fi.flatOpcode == FO::InlineReturnScalarJump) {
                op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            }
            op.handler = &th_InlineReturn;
            break;

        case FO::TermJumpNoArgs:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.handler = &th_Jump;
            break;
        case FO::TermBranchBoolNoArgs:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = &rec.threadedStream[fi.falseTargetOffset];
            op.handler = &th_BranchBool;
            break;
        case FO::TermBranchGeneric:
            op.trueTarget = &rec.threadedStream[fi.trueTargetOffset];
            op.falseTarget = (fi.falseTargetOffset < n)
                ? &rec.threadedStream[fi.falseTargetOffset]
                : nullptr;
            op.handler = &th_BranchGeneric;
            break;
        case FO::TermReturnVoid:
            op.handler = &th_ReturnVoid;
            break;
        case FO::TermReturnScalarFast:
            op.handler = &th_ReturnValue;
            break;
        case FO::TermReturnGeneric: {
            const auto& cold = execFunc.flatCold[i];
            const auto retReg = execFunc.blocks[cold.blockIndex].bytecode->terminator.value;
            op.src0Off = static_cast<std::uint32_t>(execFunc.registerOffset + retReg) * 16u;
            op.handler = &th_ReturnValue;
            break;
        }
        default:
            op.handler = &th_ScalarMisc;
            break;
        }
    }
}

[[nodiscard]] bool validateJitEntryArguments(
    const bytecode::Function& function,
    runtime::ManagedHeap* heap,
    const std::vector<runtime::Value>& arguments,
    bool& outAllIntArgsFit32,
    runtime::ErrorCode& outCode,
    std::string& outMsg) {
    outAllIntArgsFit32 = true;
    if (arguments.size() != function.parameterTypes.size()) {
        outCode = runtime::ErrorCode::InvalidArguments;
        outMsg = "argument count does not match function signature";
        return false;
    }
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto& arg = arguments[i];
        const auto expectedType = function.parameterTypes[i];
        const auto expectedTid = i < function.parameterTypeIds.size()
            ? function.parameterTypeIds[i]
            : semantic::SymbolId{0};
        if (const auto* ref = std::get_if<runtime::ObjectRef>(&arg)) {
            if (!heap || !heap->isAlive(*ref)) {
                outCode = runtime::ErrorCode::InvalidObjectReference;
                outMsg = "argument contains an invalid managed object reference";
                return false;
            }
        }
        if (runtime::valueType(arg) != expectedType) {
            outCode = runtime::ErrorCode::TypeMismatch;
            outMsg = "argument type does not match function signature";
            return false;
        }
        if (expectedType == semantic::PrimitiveType::Int) {
            if (!fitsInt32(arg.asInt64Unchecked())) {
                outAllIntArgsFit32 = false;
            }
        } else if (expectedType == semantic::PrimitiveType::Array) {
            if (!std::holds_alternative<runtime::NullArray>(arg) && expectedTid != 0) {
                const auto* ref = std::get_if<runtime::ObjectRef>(&arg);
                const auto actualTid = (ref && heap)
                    ? heap->arrayTypeId(*ref)
                    : std::optional<semantic::SymbolId>{};
                if (!actualTid || *actualTid != expectedTid) {
                    outCode = runtime::ErrorCode::TypeMismatch;
                    outMsg = "argument has the wrong runtime array type";
                    return false;
                }
            }
        } else if (expectedType == semantic::PrimitiveType::Struct) {
            if (expectedTid != 0 && runtime::structValueTypeId(arg) != expectedTid) {
                outCode = runtime::ErrorCode::TypeMismatch;
                outMsg = "argument has the wrong runtime struct type";
                return false;
            }
        } else if (expectedType == semantic::PrimitiveType::Object ||
                   expectedType == semantic::PrimitiveType::Enum ||
                   expectedType == semantic::PrimitiveType::Handle) {
            // Let full signature checker in Interpreter handle complex exact object/handle hierarchy if non-null
            if (expectedTid != 0) {
                outAllIntArgsFit32 = false;
            }
        }
    }
    return true;
}

} // namespace

// ============================================================================
// StencilModule & StencilJit Implementation
// ============================================================================

struct StencilModule::Impl {
    std::shared_ptr<const runtime::ProgramImage> program;
    std::shared_ptr<runtime::ManagedHeap> heap;
    std::shared_ptr<const runtime::BindingRegistry> bindings;
    runtime::ExternalFunction externalResolver;
    std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime;
    StencilJitOptions options{};

    ExecutableCodeArena primaryArena;
    mutable std::vector<ExecutableCodeArena> lazyTierArenas;
    mutable std::mutex tierPromotionMutex;

    std::unordered_map<semantic::SymbolId, std::unique_ptr<CompiledFunctionRecord>> records;
    mutable std::atomic<std::size_t> nativeCount{0};
    mutable std::atomic<std::size_t> threadedCount{0};
    mutable std::atomic<std::size_t> fallbackCount{0};
    mutable std::atomic<std::size_t> codeBytes{0};
    bool nativeBackendAvailable = false;

    explicit Impl(
        std::shared_ptr<const runtime::ProgramImage> prog,
        StencilJitOptions opts)
        : program(std::move(prog)),
          heap(std::make_shared<runtime::ManagedHeap>()),
          asyncRuntime(std::make_shared<runtime::AsyncRuntimeContext>(heap.get())),
          options(opts),
          primaryArena(opts.arenaBlockBytes) {}

    bool promoteFunctionToNativeLocked(CompiledFunctionRecord& rec, ExecutableCodeArena* batchArena) const {
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
        if (!nativeBackendAvailable || !rec.eligibleForNative) return false;
        if (rec.tier.load(std::memory_order_relaxed) == StencilTier::NativeMachineCode) return true;

#if defined(__aarch64__) || defined(_M_ARM64)
        auto machineBytes = compileFunctionA64(rec);
#else
        auto machineBytes = compileFunctionX64(rec);
#endif
        if (machineBytes.empty()) return false;

        if (batchArena != nullptr) {
            std::uint8_t* dst = batchArena->emit(machineBytes.data(), machineBytes.size(), 16);
            if (!dst) return false;
            rec.rawCodePtr = dst;
            rec.rawCodeSize = machineBytes.size();
            rec.nativeEntry.store(bitCastFnPtr<NativeJitEntryFn>(dst), std::memory_order_release);
            rec.tier.store(StencilTier::NativeMachineCode, std::memory_order_release);
            return true;
        }

        ExecutableCodeArena singleArena(std::max<std::size_t>(machineBytes.size() + 64u, ExecutableCodeArena::systemPageSize()));
        std::uint8_t* dst = singleArena.emit(machineBytes.data(), machineBytes.size(), 16);
        if (!dst || !singleArena.seal()) return false;

        rec.rawCodePtr = dst;
        rec.rawCodeSize = machineBytes.size();
        const StencilTier prevTier = rec.tier.load(std::memory_order_relaxed);
        rec.nativeEntry.store(bitCastFnPtr<NativeJitEntryFn>(dst), std::memory_order_release);
        rec.tier.store(StencilTier::NativeMachineCode, std::memory_order_release);
        lazyTierArenas.push_back(std::move(singleArena));

        if (prevTier == StencilTier::ThreadedStencils) {
            threadedCount.fetch_sub(1, std::memory_order_relaxed);
        } else if (prevTier == StencilTier::InterpreterFallback) {
            fallbackCount.fetch_sub(1, std::memory_order_relaxed);
        }
        nativeCount.fetch_add(1, std::memory_order_relaxed);
        codeBytes.fetch_add(machineBytes.size(), std::memory_order_relaxed);
        return true;
#else
        (void)rec;
        (void)batchArena;
        return false;
#endif
    }

    runtime::ExecutionResult invokeFallbackInterpreter(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments,
        runtime::ExecutionOptions execOptions) const {
        runtime::Interpreter interp(program, heap);
        if (bindings) interp.setBindingRegistry(bindings);
        if (externalResolver) interp.setExternalResolver(externalResolver);
        if (asyncRuntime) interp.setAsyncRuntime(asyncRuntime);
        return interp.invoke(symbolId, arguments, std::move(execOptions));
    }

    runtime::ExecutionResult invokeBySymbol(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments,
        runtime::ExecutionOptions execOptions,
        bool forceSimulateArm64 = false,
        std::size_t baseFrameOffset = 0) const {
        if (!program) {
            runtime::ExecutionResult invalid;
            invalid.error.code = runtime::ErrorCode::InvalidProgram;
            invalid.error.message = "StencilJit module has no linked program image";
            return invalid;
        }

        const auto recIt = records.find(symbolId);
        if (recIt == records.end() || !recIt->second || !recIt->second->location) {
            runtime::ExecutionResult missing;
            missing.error.code = runtime::ErrorCode::FunctionNotFound;
            missing.error.message = "entry function was not found";
            return missing;
        }

        const bool hasTraceOrDebugOrStrictEvents =
            execOptions.determinism.mode != runtime::DeterminismMode::Off ||
            execOptions.profile != nullptr ||
            (execOptions.trace && static_cast<bool>(execOptions.trace)) ||
            execOptions.debugger != nullptr;
        const bool activeGcCycle =
            execOptions.limits.gcWorkBudget != 0 &&
            heap != nullptr &&
            (heap->phase() != runtime::GcPhase::Idle || heap->collectionRequested());

        if (hasTraceOrDebugOrStrictEvents || activeGcCycle) {
            return invokeFallbackInterpreter(symbolId, arguments, std::move(execOptions));
        }

        CompiledFunctionRecord& rec = *recIt->second;
        StencilTier currentTier = rec.tier.load(std::memory_order_acquire);

        if (options.hotnessThreshold > 0 &&
            currentTier != StencilTier::NativeMachineCode &&
            rec.eligibleForNative &&
            nativeBackendAvailable) {
            const std::uint32_t calls = rec.callCount.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (calls >= options.hotnessThreshold) {
                std::lock_guard<std::mutex> lock(tierPromotionMutex);
                (void)promoteFunctionToNativeLocked(rec, nullptr);
                currentTier = rec.tier.load(std::memory_order_acquire);
            }
        }

        if (currentTier == StencilTier::InterpreterFallback) {
            return invokeFallbackInterpreter(symbolId, arguments, std::move(execOptions));
        }

        const auto& fnLoc = *rec.location;
        const auto& bcFunc = *fnLoc.function;
        const auto& execFunc = *rec.execFunc;

        if (execOptions.limits.recursionLimit == 0) {
            return invokeFallbackInterpreter(symbolId, arguments, std::move(execOptions));
        }

        bool allIntArgsFit32 = true;
        runtime::ErrorCode argErrCode = runtime::ErrorCode::None;
        std::string argErrMsg;
        if (!validateJitEntryArguments(bcFunc, heap.get(), arguments, allIntArgsFit32, argErrCode, argErrMsg)) {
            runtime::ExecutionResult argFail;
            argFail.error.code = argErrCode;
            argFail.error.message = std::move(argErrMsg);
            runtime::DeterminismSession det(std::move(execOptions.determinism));
            (void)det.finish(false, runtime::Value{}, argFail.error, argFail.statistics);
            argFail.determinismDigest = det.digest();
            return argFail;
        }
        if (!allIntArgsFit32) {
            return invokeFallbackInterpreter(symbolId, arguments, std::move(execOptions));
        }

        runtime::detail::ActiveMutatorScope activeMutatorScope;

        std::vector<runtime::Value> localFallbackArena;
        bool usingTls = false;
        std::vector<runtime::Value>* arenaPtr = &localFallbackArena;
        if (!g_stencilTlsScratch.inUse) {
            g_stencilTlsScratch.inUse = true;
            usingTls = true;
            arenaPtr = &g_stencilTlsScratch.values;
        }
        struct TlsReleaseGuard {
            bool active = false;
            ~TlsReleaseGuard() noexcept {
                if (active) {
                    if (g_stencilTlsScratch.values.size() > 4096) {
                        g_stencilTlsScratch.values.resize(256);
                    }
                    g_stencilTlsScratch.inUse = false;
                }
            }
        } tlsGuard{usingTls};

        const std::size_t frameSlots = execFunc.totalFrameSlots;
        if (baseFrameOffset > runtime::StructStorageHandle::FrameOffsetMask ||
            frameSlots > runtime::StructStorageHandle::FrameOffsetMask - baseFrameOffset) {
            runtime::ExecutionResult invalid;
            invalid.error.code = runtime::ErrorCode::InvalidProgram;
            invalid.error.message = "frameOffset exceeds 24-bit FlatStruct slice address space";
            return invalid;
        }
        const std::size_t totalSlots = baseFrameOffset + frameSlots;
        if (arenaPtr->size() < totalSlots) {
            arenaPtr->resize(std::max<std::size_t>(totalSlots, 256));
        }
        if (baseFrameOffset > 0) {
            std::memset(
                static_cast<void*>(arenaPtr->data()),
                0,
                baseFrameOffset * sizeof(runtime::Value));
        }

        runtime::Value* frameBase = arenaPtr->data() + baseFrameOffset;
        const std::size_t argCount = arguments.size();
        for (std::size_t i = 0; i < argCount; ++i) {
            frameBase[i] = arguments[i];
        }
        if (frameSlots > argCount) {
            std::memset(
                static_cast<void*>(frameBase + argCount),
                0,
                (frameSlots - argCount) * sizeof(runtime::Value));
        }

        JitRuntimeSession session{};
        session.program = program.get();
        session.heap = heap.get();
        session.limits = execOptions.limits;
        session.stats.functionCalls = 1;
        session.stats.maximumCallDepth = 1;
        session.values = arenaPtr;
        session.frameOffset = baseFrameOffset;
        session.valueTop = totalSlots;
        session.callDepth = 1;
        session.valuesBase = arenaPtr->data();
        session.frameOffset32 = static_cast<std::uint32_t>(baseFrameOffset);
        session.flatBankId = runtime::detail::acquireFlatStackBank(arenaPtr);
        struct JitFlatBankGuard {
            std::uint8_t& bankId;
            ~JitFlatBankGuard() noexcept {
                if (bankId != 0u) {
                    runtime::detail::releaseFlatStackBank(bankId);
                    bankId = 0u;
                }
            }
        } jitBankGuard{session.flatBankId};

        if (execFunc.hasStructPayloads && session.flatBankId != 0u) {
            for (std::size_t i = 0; i < argCount; ++i) {
                const auto pSlot = static_cast<std::uint16_t>(execFunc.paramOffset + i);
                if (pSlot < execFunc.slotStructSchemaIds.size() && execFunc.slotStructSchemaIds[pSlot] != 0u) {
                    const std::uint16_t sid = execFunc.slotStructSchemaIds[pSlot];
                    const std::uint16_t payOff = execFunc.slotPayloadOffsets[pSlot];
                    const auto* sc = runtime::getFlatStructSchema(sid);
                    if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg) {
                        const runtime::Value incoming = frameBase[pSlot];
                        if (runtime::extractStructLeavesToBuffer(incoming, *sc, frameBase + payOff)) {
                            frameBase[pSlot] = runtime::StructValue::makeFrameSlice(
                                sc->structTypeId,
                                session.flatBankId,
                                static_cast<std::uint32_t>(baseFrameOffset + payOff),
                                sid);
                        } else if (incoming.rawTag() == runtime::ValueTag::Monostate) {
                            std::memcpy(
                                static_cast<void*>(frameBase + payOff),
                                static_cast<const void*>(sc->prototypeLeaves.data()),
                                static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                            frameBase[pSlot] = runtime::StructValue::makeFrameSlice(
                                sc->structTypeId,
                                session.flatBankId,
                                static_cast<std::uint32_t>(baseFrameOffset + payOff),
                                sid);
                        }
                    }
                }
            }
            const std::size_t nLocals = bcFunc.localTypes.size();
            for (std::size_t l = 0; l < nLocals; ++l) {
                const auto lSlot = static_cast<std::uint16_t>(execFunc.localOffset + l);
                if (lSlot < execFunc.slotStructSchemaIds.size() && execFunc.slotStructSchemaIds[lSlot] != 0u) {
                    const std::uint16_t sid = execFunc.slotStructSchemaIds[lSlot];
                    const std::uint16_t payOff = execFunc.slotPayloadOffsets[lSlot];
                    const auto* sc = runtime::getFlatStructSchema(sid);
                    if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg) {
                        std::memcpy(
                            static_cast<void*>(frameBase + payOff),
                            static_cast<const void*>(sc->prototypeLeaves.data()),
                            static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                        frameBase[lSlot] = runtime::StructValue::makeFrameSlice(
                            sc->structTypeId,
                            session.flatBankId,
                            static_cast<std::uint32_t>(baseFrameOffset + payOff),
                            sid);
                    }
                }
            }
        }
        session.shadowStack.pushFrame(nullptr, nullptr, arenaPtr, &session.valueTop);

        runtime::Value retValue{};
        JitAbiContext abi{};
        abi.frameBase = frameBase;
        abi.remainingBudget = execOptions.limits.instructionBudget;
        abi.branchesTaken = 0;
        abi.returnSlot = &retValue;
        abi.errorFlatIndex = 0;
        abi.errorCodeRaw = 0;
        abi.session = &session;
        abi.compiledFn = &rec;
        invalidateJitArrayCaches(abi);

        std::uint32_t rawStatus = 0u;
        std::string simDiag;
        if (forceSimulateArm64) {
#if UINTPTR_MAX == 0xffffffffffffffffULL
            const auto arm64Code = compileFunctionA64(rec);
            if (arm64Code.empty()) {
                simDiag = "compileFunctionA64 returned empty machine code";
                rawStatus = static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
            } else {
                rawStatus = executeSimulatedArm64Code(arm64Code, &abi, simDiag);
            }
#else
            simDiag = "ARM64 StencilJit requires 64-bit host pointers";
            rawStatus = static_cast<std::uint32_t>(runtime::ErrorCode::InvalidProgram);
#endif
        } else if (currentTier == StencilTier::NativeMachineCode) {
            const NativeJitEntryFn fn = rec.nativeEntry.load(std::memory_order_acquire);
            rawStatus = fn(&abi);
        } else {
            const ThreadedStencilOp* pc = rec.threadedStream.data() + execFunc.entryStreamOffset;
            StencilStepStatus st = StencilStepStatus::Continue;
            while ((st = pc->handler(&abi, pc)) == StencilStepStatus::Continue) {}
            rawStatus = (st == StencilStepStatus::ReturnOk) ? 0u : abi.errorCodeRaw;
        }

        retValue = runtime::detachFrameSliceValue(retValue);
        session.shadowStack.popFrame();

        const std::uint64_t executed =
            execOptions.limits.instructionBudget >= abi.remainingBudget
                ? (execOptions.limits.instructionBudget - abi.remainingBudget)
                : execOptions.limits.instructionBudget;
        session.stats.instructionsExecuted = executed;
        session.stats.branchesTaken = abi.branchesTaken;

        bool succeeded = (rawStatus == 0u);
        if (!succeeded && session.error.code == runtime::ErrorCode::None) {
            const auto errCode = static_cast<runtime::ErrorCode>(rawStatus);
            const char* msg = "runtime error in StencilJit execution";
            switch (errCode) {
            case runtime::ErrorCode::InstructionBudgetExceeded:
                msg = "instruction budget exceeded";
                break;
            case runtime::ErrorCode::IntegerOverflow:
                msg = "integer overflow";
                break;
            case runtime::ErrorCode::DivisionByZero:
                msg = "division by zero";
                break;
            case runtime::ErrorCode::TypeMismatch:
                msg = "expected matching scalar type in StencilJit instruction";
                break;
            default:
                break;
            }
            populateJitErrorDiagnostics(
                session,
                fnLoc,
                execFunc,
                abi.errorFlatIndex,
                errCode,
                simDiag.empty() ? std::string(msg) : simDiag);
        }

        runtime::DeterminismSession det(std::move(execOptions.determinism));
        succeeded = det.finish(succeeded, retValue, session.error, session.stats);

        runtime::ExecutionResult out;
        out.succeeded = succeeded;
        out.value = std::move(retValue);
        out.error = std::move(session.error);
        out.instructionsExecuted = executed;
        out.statistics = session.stats;
        out.determinismDigest = det.digest();
        out.replayEntriesConsumed = det.replayEntriesConsumed();
        return out;
    }
};

StencilModule::StencilModule() = default;
StencilModule::StencilModule(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StencilModule::~StencilModule() = default;
StencilModule::StencilModule(StencilModule&&) noexcept = default;
StencilModule& StencilModule::operator=(StencilModule&&) noexcept = default;

bool StencilModule::valid() const noexcept {
    return impl_ && impl_->program != nullptr;
}

const runtime::ProgramImage* StencilModule::program() const noexcept {
    return impl_ ? impl_->program.get() : nullptr;
}

std::shared_ptr<const runtime::ProgramImage> StencilModule::programSnapshot() const noexcept {
    return impl_ ? impl_->program : nullptr;
}

void StencilModule::setBindings(std::shared_ptr<const runtime::BindingRegistry> bindings) {
    if (impl_) impl_->bindings = std::move(bindings);
}

void StencilModule::setBindingRegistry(std::shared_ptr<const runtime::BindingRegistry> bindings) {
    setBindings(std::move(bindings));
}

void StencilModule::setExternalResolver(runtime::ExternalFunction resolver) {
    if (impl_) impl_->externalResolver = std::move(resolver);
}

void StencilModule::setHeap(std::shared_ptr<runtime::ManagedHeap> heap) {
    if (!impl_) return;
    impl_->heap = heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>();
    if (impl_->asyncRuntime) {
        impl_->asyncRuntime->setHeap(impl_->heap.get());
    }
}

std::shared_ptr<runtime::ManagedHeap> StencilModule::heap() const noexcept {
    return impl_ ? impl_->heap : nullptr;
}

void StencilModule::setAsyncRuntime(std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime) {
    if (!impl_) return;
    impl_->asyncRuntime = asyncRuntime
        ? std::move(asyncRuntime)
        : std::make_shared<runtime::AsyncRuntimeContext>(impl_->heap.get());
    if (impl_->heap && impl_->asyncRuntime->awaitables().heap() == nullptr) {
        impl_->asyncRuntime->setHeap(impl_->heap.get());
    }
}

std::shared_ptr<runtime::AsyncRuntimeContext> StencilModule::asyncRuntime() const noexcept {
    return impl_ ? impl_->asyncRuntime : nullptr;
}

runtime::ExecutionResult StencilModule::invoke(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult invalid;
        invalid.error.code = runtime::ErrorCode::InvalidProgram;
        invalid.error.message = "StencilJit module is not loaded";
        return invalid;
    }
    if (const auto symId = impl_->program->findFunction(qualifiedName)) {
        return impl_->invokeBySymbol(*symId, arguments, std::move(options));
    }
    runtime::ExecutionResult missing;
    missing.error.code = runtime::ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

runtime::ExecutionResult StencilModule::invoke(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::Limits limits) const {
    runtime::ExecutionOptions opts;
    opts.limits = limits;
    return invoke(qualifiedName, arguments, std::move(opts));
}

runtime::ExecutionResult StencilModule::invoke(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult invalid;
        invalid.error.code = runtime::ErrorCode::InvalidProgram;
        invalid.error.message = "StencilJit module is not loaded";
        return invalid;
    }
    return impl_->invokeBySymbol(symbolId, arguments, std::move(options));
}

runtime::ExecutionResult StencilModule::invoke(
    semantic::SymbolId symbolId,
    const std::vector<runtime::Value>& arguments,
    runtime::Limits limits) const {
    runtime::ExecutionOptions opts;
    opts.limits = limits;
    return invoke(symbolId, arguments, std::move(opts));
}

std::size_t StencilModule::totalFunctionCount() const noexcept {
    return impl_ ? impl_->records.size() : 0;
}

std::size_t StencilModule::nativeFunctionCount() const noexcept {
    return impl_ ? impl_->nativeCount.load(std::memory_order_relaxed) : 0;
}

std::size_t StencilModule::threadedFunctionCount() const noexcept {
    return impl_ ? impl_->threadedCount.load(std::memory_order_relaxed) : 0;
}

std::size_t StencilModule::fallbackFunctionCount() const noexcept {
    return impl_ ? impl_->fallbackCount.load(std::memory_order_relaxed) : 0;
}

std::size_t StencilModule::machineCodeBytes() const noexcept {
    return impl_ ? impl_->codeBytes.load(std::memory_order_relaxed) : 0;
}

bool StencilModule::nativeBackendActive() const noexcept {
    return impl_ && impl_->nativeBackendAvailable;
}

StencilTier StencilModule::functionTier(semantic::SymbolId symbolId) const noexcept {
    if (!impl_) return StencilTier::InterpreterFallback;
    const auto it = impl_->records.find(symbolId);
    return it == impl_->records.end()
        ? StencilTier::InterpreterFallback
        : it->second->tier.load(std::memory_order_acquire);
}

StencilTier StencilModule::functionTier(const std::string& qualifiedName) const noexcept {
    if (!valid()) return StencilTier::InterpreterFallback;
    const auto sym = impl_->program->findFunction(qualifiedName);
    return sym ? functionTier(*sym) : StencilTier::InterpreterFallback;
}

StencilTargetArch StencilModule::hostTargetArch() const noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return StencilTargetArch::Arm64;
#elif defined(__x86_64__) || defined(_M_X64)
    return StencilTargetArch::X86_64;
#else
    return StencilTargetArch::Host;
#endif
}

std::vector<std::uint8_t> StencilModule::inspectMachineCode(
    const std::string& qualifiedName) const {
    return inspectMachineCode(qualifiedName, StencilTargetArch::Host);
}

std::vector<std::uint8_t> StencilModule::inspectMachineCode(
    const std::string& qualifiedName,
    StencilTargetArch targetArch) const {
    if (!valid()) return {};
    const auto sym = impl_->program->findFunction(qualifiedName);
    if (!sym) return {};
    const auto it = impl_->records.find(*sym);
    if (it == impl_->records.end() || !it->second) {
        return {};
    }
    const CompiledFunctionRecord& rec = *it->second;
    if (targetArch == StencilTargetArch::Host || targetArch == hostTargetArch()) {
        if (rec.rawCodePtr && rec.rawCodeSize > 0) {
            return std::vector<std::uint8_t>(rec.rawCodePtr, rec.rawCodePtr + rec.rawCodeSize);
        }
    }
#if UINTPTR_MAX == 0xffffffffffffffffULL
    if (!rec.eligibleForNative || rec.execFunc == nullptr) {
        return {};
    }
    if (targetArch == StencilTargetArch::Arm64) {
        return compileFunctionA64(rec);
    }
    if (targetArch == StencilTargetArch::X86_64) {
        return compileFunctionX64(rec);
    }
#endif
    return {};
}

runtime::ExecutionResult StencilModule::invokeSimulatedArm64(
    const std::string& qualifiedName,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult invalid;
        invalid.error.code = runtime::ErrorCode::InvalidProgram;
        invalid.error.message = "StencilJit module is not loaded";
        return invalid;
    }
    if (const auto symId = impl_->program->findFunction(qualifiedName)) {
        return impl_->invokeBySymbol(*symId, arguments, std::move(options), true);
    }
    runtime::ExecutionResult missing;
    missing.error.code = runtime::ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

runtime::ExecutionResult StencilModule::invokeWithBaseFrameOffset(
    const std::string& qualifiedName,
    std::size_t baseFrameOffset,
    bool simulateArm64,
    const std::vector<runtime::Value>& arguments,
    runtime::ExecutionOptions options) const {
    if (!valid()) {
        runtime::ExecutionResult invalid;
        invalid.error.code = runtime::ErrorCode::InvalidProgram;
        invalid.error.message = "StencilJit module is not loaded";
        return invalid;
    }
    if (const auto symId = impl_->program->findFunction(qualifiedName)) {
        return impl_->invokeBySymbol(*symId, arguments, std::move(options), simulateArm64, baseFrameOffset);
    }
    runtime::ExecutionResult missing;
    missing.error.code = runtime::ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

bool StencilJit::nativeAvailable() noexcept {
    return ValueLayoutProbe::verify() &&
           ExecutableCodeArena::nativeCodeExecutionSupported();
}

StencilCompileResult StencilJit::compile(
    std::vector<bytecode::Module> modules,
    StencilJitOptions options) const {
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(modules), linkErr);
    if (!linked) {
        StencilCompileResult failed;
        failed.error = linkErr.message.empty()
            ? "failed to link bytecode modules for StencilJit"
            : linkErr.message;
        return failed;
    }
    return compile(std::make_shared<const runtime::ProgramImage>(std::move(*linked)), options);
}

StencilCompileResult StencilJit::compile(
    std::shared_ptr<const runtime::ProgramImage> program,
    StencilJitOptions options) const {
    const auto tStart = std::chrono::steady_clock::now();
    StencilCompileResult result;

    if (!program) {
        result.error = "StencilJit::compile received a null ProgramImage";
        return result;
    }
    if (!ValueLayoutProbe::verify()) {
        result.error = "runtime::Value ABI layout verification failed";
        return result;
    }

    auto impl = std::make_unique<StencilModule::Impl>(program, options);
    const bool canEmitNative =
        options.enableNativeCodegen && ExecutableCodeArena::nativeCodeExecutionSupported();
    impl->nativeBackendAvailable = canEmitNative;

    const auto& mirror = runtime::detail::executionMirror(*program);
    std::vector<CompiledFunctionRecord*> eagerNativeQueue;

    for (const auto& module : program->modules()) {
        for (const auto& func : module.functions) {
            const auto* fnLoc = program->resolveFunction(func.symbolId);
            if (!fnLoc) continue;
            const auto* execFunc = runtime::detail::findExecutionFunction(mirror, func);

            auto rec = std::make_unique<CompiledFunctionRecord>();
            rec->symbolId = func.symbolId;
            rec->location = fnLoc;
            rec->execFunc = execFunc;

            if (execFunc != nullptr && isFunctionEligibleForStencilJit(*execFunc)) {
                rec->unexecutedAfterOp.resize(execFunc->flatStream.size(), 0u);
                for (std::size_t idx = 0; idx < execFunc->flatStream.size(); ++idx) {
                    rec->unexecutedAfterOp[idx] = computeUnexecutedAfterOp(*execFunc, idx);
                }
                rec->eligibleForNative = true;
                rec->eligibleForThreaded = options.enableThreadedFallback;

                if (canEmitNative && options.hotnessThreshold == 0) {
                    eagerNativeQueue.push_back(rec.get());
                } else if (options.enableThreadedFallback) {
                    buildThreadedStream(*rec);
                    rec->tier.store(StencilTier::ThreadedStencils, std::memory_order_relaxed);
                } else {
                    rec->tier.store(StencilTier::InterpreterFallback, std::memory_order_relaxed);
                }
            } else {
                rec->tier.store(StencilTier::InterpreterFallback, std::memory_order_relaxed);
            }

            impl->records.emplace(func.symbolId, std::move(rec));
        }
    }

    if (!eagerNativeQueue.empty()) {
        for (auto* rec : eagerNativeQueue) {
            if (!impl->promoteFunctionToNativeLocked(*rec, &impl->primaryArena)) {
                if (options.enableThreadedFallback) {
                    buildThreadedStream(*rec);
                    rec->tier.store(StencilTier::ThreadedStencils, std::memory_order_relaxed);
                } else {
                    rec->tier.store(StencilTier::InterpreterFallback, std::memory_order_relaxed);
                }
            }
        }
        if (impl->primaryArena.allocatedBytes() > 0 && !impl->primaryArena.seal()) {
            // Restricted W^X environment at runtime: gracefully downgrade all functions to ThreadedStencils / Fallback!
            impl->nativeBackendAvailable = false;
            for (auto* rec : eagerNativeQueue) {
                rec->nativeEntry.store(nullptr, std::memory_order_relaxed);
                rec->rawCodePtr = nullptr;
                rec->rawCodeSize = 0;
                if (options.enableThreadedFallback) {
                    buildThreadedStream(*rec);
                    rec->tier.store(StencilTier::ThreadedStencils, std::memory_order_relaxed);
                } else {
                    rec->tier.store(StencilTier::InterpreterFallback, std::memory_order_relaxed);
                }
            }
        }
    }

    std::size_t nNative = 0;
    std::size_t nThreaded = 0;
    std::size_t nFallback = 0;
    for (const auto& [sym, rec] : impl->records) {
        (void)sym;
        switch (rec->tier.load(std::memory_order_relaxed)) {
        case StencilTier::NativeMachineCode: ++nNative; break;
        case StencilTier::ThreadedStencils: ++nThreaded; break;
        case StencilTier::InterpreterFallback: ++nFallback; break;
        }
    }
    const std::size_t codeSize = impl->nativeBackendAvailable
        ? impl->primaryArena.allocatedBytes()
        : 0u;
    impl->nativeCount.store(nNative, std::memory_order_relaxed);
    impl->threadedCount.store(nThreaded, std::memory_order_relaxed);
    impl->fallbackCount.store(nFallback, std::memory_order_relaxed);
    impl->codeBytes.store(codeSize, std::memory_order_relaxed);

    const auto tStop = std::chrono::steady_clock::now();
    result.compileTimeMicroseconds =
        std::chrono::duration<double, std::micro>(tStop - tStart).count();
    result.totalFunctionCount = impl->records.size();
    result.nativeFunctionCount = nNative;
    result.threadedFunctionCount = nThreaded;
    result.fallbackFunctionCount = nFallback;
    result.machineCodeBytes = codeSize;
    result.nativeBackendActive = impl->nativeBackendAvailable;
    result.module = std::shared_ptr<StencilModule>(new StencilModule(std::move(impl)));
    return result;
}

} // namespace realscript::jit
