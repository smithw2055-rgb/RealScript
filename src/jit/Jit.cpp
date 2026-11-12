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

[[nodiscard]] std::uint8_t* osAllocateRwPages(std::size_t byteSize) noexcept {
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
    void* mem = mmap(nullptr, byteSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | kAnonFlag, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    return static_cast<std::uint8_t*>(mem);
#endif
}

[[nodiscard]] bool osProtectRxPages(std::uint8_t* base, std::size_t byteSize) noexcept {
    if (!base || byteSize == 0) return false;
#if defined(_WIN32)
    DWORD oldProtect = 0;
    if (VirtualProtect(base, byteSize, PAGE_EXECUTE_READ, &oldProtect) == 0) {
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), base, byteSize);
    return true;
#else
    if (mprotect(base, byteSize, PROT_READ | PROT_EXEC) != 0) {
        return false;
    }
#if defined(__GNUC__) || defined(__clang__)
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
    const std::size_t align = std::max<std::size_t>(alignment, 1);

    if (!blocks_.empty() && !blocks_.back().executable) {
        auto& cur = blocks_.back();
        const std::size_t alignedOff = alignUp(cur.used, align);
        if (alignedOff <= cur.capacity && size <= cur.capacity - alignedOff) {
            if (alignedOff > cur.used) {
                std::memset(cur.base + cur.used, 0xCC, alignedOff - cur.used);
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
    std::uint8_t* base = osAllocateRwPages(allocCap);
    if (!base) return nullptr;

    Block blk{};
    blk.base = base;
    blk.capacity = allocCap;
    blk.used = size;
    blk.executable = false;
    std::memcpy(blk.base, code, size);
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
            std::memset(blk.base + blk.used, 0xCC, blk.capacity - blk.used);
        }
        if (!osProtectRxPages(blk.base, blk.capacity)) {
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
    std::uint64_t slotAndGen;     // offset 0  (0xffffffffffffffffULL when invalid)
    std::uint32_t heapId;         // offset 8
    std::uint8_t kind;            // offset 12
    std::uint8_t isPacked;        // offset 13
    std::uint8_t encoding;        // offset 14 (PackedElemEncoding)
    std::uint8_t elementType;     // offset 15 (semantic::PrimitiveType)
    void* rawData;                // offset 16 (packed rawData or boxed Value* data)
    std::uint32_t length;         // offset 24
    std::uint32_t pad0;           // offset 28
    std::uint64_t elementTypeId;  // offset 32
    std::uint64_t pad1;           // offset 40
    std::uint64_t pad2;           // offset 48
    std::uint64_t pad3;           // offset 56
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
static_assert(offsetof(JitArrayCacheEntry, elementTypeId) == 32);

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
        ce.elementTypeId = 0;
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
        ce.elementTypeId = static_cast<std::uint64_t>(pView.elementTypeId);
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
        ce.elementTypeId = static_cast<std::uint64_t>(bView.elementTypeId);
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
    } else if (fi.aux0Slot >= 2 && fi.aux0Slot <= 6) {
        if (cold.bytecode) elemTid = cold.bytecode->elementTypeId;
        switch (fi.aux0Slot) {
        case 2: initVal = runtime::FixedValue{}; break;
        case 3: initVal = runtime::Fixed16Value{}; break;
        case 4: initVal = runtime::Vec2Value{}; break;
        case 5: initVal = runtime::Vec3Value{}; break;
        default: initVal = runtime::QuatValue{}; break;
        }
    } else if (cold.bytecode) {
        elemTid = cold.bytecode->elementTypeId;
        if (elemType == semantic::PrimitiveType::Enum) {
            initVal = runtime::EnumValue{elemTid, 0};
        } else if (elemType == semantic::PrimitiveType::Array) {
            initVal = runtime::NullArray{};
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

    const auto loaded = session.heap->arrayGet(ref, static_cast<std::size_t>(idx));
    if (!loaded) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "array element load failed");
    }

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    ctx->remainingBudget -= fi.span;
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

    semantic::PrimitiveType expectedType = semantic::PrimitiveType::Int;
    if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementPrimFast) {
        expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1));
    } else if (cold.bytecode) {
        expectedType = cold.bytecode->elementType;
    }

    if (runtime::valueType(storeVal) != expectedType) {
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

    runtime::RuntimeError setErr;
    if (!session.heap->arraySet(ref, static_cast<std::size_t>(idx), storeVal, &setErr)) {
        ctx->remainingBudget -= opDoneCost;
        const auto code = setErr.code == runtime::ErrorCode::None
            ? runtime::ErrorCode::TypeMismatch
            : setErr.code;
        return setHelperError(ctx, flatIdx, code, setErr.message.empty() ? "arraySet failed" : setErr.message);
    }

    ctx->remainingBudget -= fi.span;
    // Re-bind cache slot in case PackedI32 widened to PackedI64
    (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
    return 0u;
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
    case FO::NewArrayFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
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
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.src2Slot);
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
// P2-A Tier 2: Native x86_64 Copy-and-Patch / Stencil Machine Code Emitter
// ============================================================================

#if defined(__x86_64__) || defined(_M_X64)

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
    };

    Kind kind = Kind::BudgetSimple;
    std::uint32_t stubLabel = 0;
    std::uint32_t resumeLabel = 0;
    std::uint32_t altResumeLabel = 0;
    std::uint32_t flatIdx = 0;
    std::int32_t restoreBudget = 0;
};

[[nodiscard]] std::vector<std::uint8_t> compileFunctionX64(
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
            emitBudgetDeduct(i32Idx, fi.span, false);
            if (fi.dstSlot != fi.src0Slot) {
                as.movupsLoad(0, X64Reg::RBX, slotOff(fi.src0Slot));
                as.movupsStore(X64Reg::RBX, slotOff(fi.dstSlot), 0);
            }
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
    as.pop(X64Reg::RBX);
    as.pop(X64Reg::RBP);
    as.ret();

    if (!as.allLabelsBound()) return {};
    return as.bytes();
}

#endif // defined(__x86_64__) || defined(_M_X64)

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
        case FO::MoveSlot: op.handler = &th_MoveSlot; break;
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
        } else if (expectedType == semantic::PrimitiveType::Object ||
                   expectedType == semantic::PrimitiveType::Struct ||
                   expectedType == semantic::PrimitiveType::Enum ||
                   expectedType == semantic::PrimitiveType::Handle) {
            // Let full signature checker in Interpreter handle complex exact object/struct/handle hierarchy if non-null
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
#if defined(__x86_64__) || defined(_M_X64)
        if (!nativeBackendAvailable || !rec.eligibleForNative) return false;
        if (rec.tier.load(std::memory_order_relaxed) == StencilTier::NativeMachineCode) return true;

        auto machineBytes = compileFunctionX64(rec);
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
        runtime::ExecutionOptions execOptions) const {
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

        const std::size_t totalSlots = execFunc.totalFrameSlots;
        if (arenaPtr->size() < totalSlots) {
            arenaPtr->resize(std::max<std::size_t>(totalSlots, 256));
        }

        runtime::Value* frameBase = arenaPtr->data();
        const std::size_t argCount = arguments.size();
        for (std::size_t i = 0; i < argCount; ++i) {
            frameBase[i] = arguments[i];
        }
        if (totalSlots > argCount) {
            std::memset(
                static_cast<void*>(frameBase + argCount),
                0,
                (totalSlots - argCount) * sizeof(runtime::Value));
        }

        JitRuntimeSession session{};
        session.program = program.get();
        session.heap = heap.get();
        session.limits = execOptions.limits;
        session.stats.functionCalls = 1;
        session.stats.maximumCallDepth = 1;
        session.values = arenaPtr;
        session.frameOffset = 0;
        session.valueTop = totalSlots;
        session.callDepth = 1;
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
        if (currentTier == StencilTier::NativeMachineCode) {
            const NativeJitEntryFn fn = rec.nativeEntry.load(std::memory_order_acquire);
            rawStatus = fn(&abi);
        } else {
            const ThreadedStencilOp* pc = rec.threadedStream.data() + execFunc.entryStreamOffset;
            StencilStepStatus st = StencilStepStatus::Continue;
            while ((st = pc->handler(&abi, pc)) == StencilStepStatus::Continue) {}
            rawStatus = (st == StencilStepStatus::ReturnOk) ? 0u : abi.errorCodeRaw;
        }

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
                session, fnLoc, execFunc, abi.errorFlatIndex, errCode, msg);
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

std::vector<std::uint8_t> StencilModule::inspectMachineCode(
    const std::string& qualifiedName) const {
    if (!valid()) return {};
    const auto sym = impl_->program->findFunction(qualifiedName);
    if (!sym) return {};
    const auto it = impl_->records.find(*sym);
    if (it == impl_->records.end() || !it->second->rawCodePtr || it->second->rawCodeSize == 0) {
        return {};
    }
    return std::vector<std::uint8_t>(
        it->second->rawCodePtr,
        it->second->rawCodePtr + it->second->rawCodeSize);
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
