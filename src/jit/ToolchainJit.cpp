#include "realscript/jit/Jit.h"

#include "../runtime/ExecutionMirror.h"
#include "realscript/aot_cpp/RuntimeAbi.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
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
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
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

bool ensureFileContent(
    const std::filesystem::path& path,
    const std::string& content,
    std::string& errorMessage) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (!ec && std::filesystem::is_regular_file(status)) {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            std::ostringstream buffer;
            buffer << existing.rdbuf();
            if ((existing.good() || existing.eof()) && buffer.str() == content) {
                return true;
            }
        }
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code dirEc;
        std::filesystem::create_directories(parent, dirEc);
        if (dirEc) {
            errorMessage = "failed to create JIT directory '" + parent.string() +
                "': " + dirEc.message();
            return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        errorMessage = "failed to open JIT file '" + path.string() + "' for writing";
        return false;
    }
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    stream.flush();
    if (!stream) {
        errorMessage = "failed to write JIT file '" + path.string() + "'";
        return false;
    }
    return true;
}

#if defined(_WIN32)
std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), needed);
    return wide;
}

// CommandLineToArgvW-compliant quoting: doubles backslashes immediately before '"'
// and before the closing '"', preventing trailing '\' in paths from escaping quotes.
std::wstring quoteWindowsCommandLineArg(std::wstring_view arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring quoted;
    quoted.reserve(arg.size() + 4u);
    quoted.push_back(L'"');
    std::size_t backslashes = 0u;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            quoted.append(backslashes * 2u + 1u, L'\\');
            quoted.push_back(L'"');
            backslashes = 0u;
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(ch);
            backslashes = 0u;
        }
    }
    quoted.append(backslashes * 2u, L'\\');
    quoted.push_back(L'"');
    return quoted;
}
#endif

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

#if defined(_WIN32)
bool startWindowsProcessRedirected(
    const wchar_t* applicationPath,
    std::vector<wchar_t>& mutableCommandLine,
    HANDLE stdOutAndErrHandle,
    LPVOID environmentBlock,
    DWORD extraCreationFlags,
    PROCESS_INFORMATION& outPi) {
    if (!applicationPath || stdOutAndErrHandle == nullptr ||
        stdOutAndErrHandle == INVALID_HANDLE_VALUE) {
        return false;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    HANDLE nulIn = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (nulIn == INVALID_HANDLE_VALUE) {
        return false;
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<std::uint8_t> attrStorage(attrSize);
    auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStorage.data());
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
        CloseHandle(nulIn);
        return false;
    }

    HANDLE inheritableHandles[2] = {nulIn, stdOutAndErrHandle};
    if (!UpdateProcThreadAttribute(
            attrList,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritableHandles,
            sizeof(inheritableHandles),
            nullptr,
            nullptr)) {
        DeleteProcThreadAttributeList(attrList);
        CloseHandle(nulIn);
        return false;
    }

    STARTUPINFOEXW siEx{};
    siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    siEx.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siEx.StartupInfo.hStdInput = nulIn;
    siEx.StartupInfo.hStdOutput = stdOutAndErrHandle;
    siEx.StartupInfo.hStdError = stdOutAndErrHandle;
    siEx.lpAttributeList = attrList;

    outPi = {};
    const BOOL created = CreateProcessW(
        applicationPath,
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | extraCreationFlags,
        environmentBlock,
        nullptr,
        &siEx.StartupInfo,
        &outPi);

    DeleteProcThreadAttributeList(attrList);
    CloseHandle(nulIn);
    return created != FALSE;
}

std::wstring resolveComSpecPath() {
    const DWORD needed = GetEnvironmentVariableW(L"COMSPEC", nullptr, 0);
    if (needed > 1) {
        std::wstring value(static_cast<std::size_t>(needed), L'\0');
        const DWORD written = GetEnvironmentVariableW(L"COMSPEC", value.data(), needed);
        if (written > 0 && written < needed) {
            value.resize(written);
            std::error_code ec;
            if (std::filesystem::is_regular_file(std::filesystem::path(value), ec)) {
                return value;
            }
        }
    }
    wchar_t sysDir[MAX_PATH]{};
    const UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (sysLen > 0 && sysLen < MAX_PATH) {
        return (std::filesystem::path(std::wstring(sysDir, sysLen)) / L"cmd.exe").wstring();
    }
    return L"cmd.exe";
}

bool resolveMsvcEnvironmentBlock(
    const std::filesystem::path& compiler,
    std::vector<wchar_t>& outEnvBlock) {
    outEnvBlock.clear();
    if (!looksLikeMsvc(compiler) ||
        GetEnvironmentVariableW(L"INCLUDE", nullptr, 0) != 0) {
        return false;
    }

    std::filesystem::path vcvars;
    auto dir = compiler.parent_path();
    for (int depth = 0; depth < 8 && !dir.empty(); ++depth) {
        const auto candidate = dir / "Auxiliary" / "Build" / "vcvars64.bat";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            vcvars = candidate;
            break;
        }
        const auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    if (vcvars.empty()) {
        return false;
    }

    std::error_code canonEc;
    const auto canonVcvars = std::filesystem::weakly_canonical(vcvars, canonEc);
    const std::wstring cacheKey = (!canonEc && !canonVcvars.empty())
        ? canonVcvars.wstring()
        : vcvars.wstring();

    static std::mutex cacheMutex;
    static std::unordered_map<std::wstring, std::vector<wchar_t>> cache;

    std::lock_guard<std::mutex> lock(cacheMutex);
    if (const auto it = cache.find(cacheKey); it != cache.end()) {
        outEnvBlock = it->second;
        return !outEnvBlock.empty();
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        return false;
    }
    if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        return false;
    }

    const std::wstring cmdPath = resolveComSpecPath();
    std::wstring cmdLine =
        quoteWindowsCommandLineArg(cmdPath) +
        L" /U /D /S /C \"call \"" + vcvars.wstring() + L"\" >nul 2>&1 && set\"";
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    const bool launched = startWindowsProcessRedirected(
        cmdPath.c_str(), mutableCmd, writePipe, nullptr, 0, pi);
    CloseHandle(writePipe);
    if (!launched) {
        CloseHandle(readPipe);
        return false;
    }

    std::string rawBytes;
    char chunk[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead > 0) {
        rawBytes.append(chunk, bytesRead);
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode != 0 || rawBytes.size() < 2u) {
        return false;
    }

    const std::size_t charCount = rawBytes.size() / sizeof(wchar_t);
    std::wstring envText(charCount, L'\0');
    std::memcpy(envText.data(), rawBytes.data(), charCount * sizeof(wchar_t));
    if (!envText.empty() && envText.front() == L'\xFEFF') {
        envText.erase(envText.begin());
    }

    std::vector<std::pair<std::wstring, std::wstring>> vars;
    std::size_t start = 0;
    while (start < envText.size()) {
        std::size_t end = envText.find_first_of(L"\r\n", start);
        if (end == std::wstring::npos) end = envText.size();
        const std::wstring_view line(envText.data() + start, end - start);
        if (!line.empty()) {
            const std::size_t eq = line.find(L'=');
            if (eq != std::wstring_view::npos && eq > 0u) {
                vars.emplace_back(
                    std::wstring(line.substr(0, eq)),
                    std::wstring(line));
            }
        }
        start = end + 1u;
    }

    std::sort(
        vars.begin(),
        vars.end(),
        [](const auto& a, const auto& b) {
            return CompareStringOrdinal(
                       a.first.data(), static_cast<int>(a.first.size()),
                       b.first.data(), static_cast<int>(b.first.size()), TRUE) == CSTR_LESS_THAN;
        });

    bool hasInclude = false;
    std::vector<wchar_t> block;
    for (std::size_t i = 0; i < vars.size(); ++i) {
        // Deduplicate identical variable names case-insensitively (keep latest sorted entry)
        if (i + 1u < vars.size() &&
            CompareStringOrdinal(
                vars[i].first.data(), static_cast<int>(vars[i].first.size()),
                vars[i + 1u].first.data(), static_cast<int>(vars[i + 1u].first.size()), TRUE) ==
                CSTR_EQUAL) {
            continue;
        }
        if (CompareStringOrdinal(
                vars[i].first.data(), static_cast<int>(vars[i].first.size()),
                L"INCLUDE", 7, TRUE) == CSTR_EQUAL) {
            hasInclude = true;
        }
        block.insert(block.end(), vars[i].second.begin(), vars[i].second.end());
        block.push_back(L'\0');
    }
    if (!hasInclude || block.empty()) {
        return false;
    }
    block.push_back(L'\0');

    cache.emplace(cacheKey, block);
    outEnvBlock = std::move(block);
    return true;
}
#endif

int invokeCompilerSubprocess(
    const ToolchainOptions& options,
    const std::filesystem::path& source,
    const std::filesystem::path& library,
    const std::filesystem::path& log) {
#if defined(_WIN32)
    std::vector<std::wstring> args;
    args.push_back(options.compiler.wstring());
    if (looksLikeMsvc(options.compiler)) {
        args.push_back(L"/nologo");
        args.push_back(L"/std:c++17");
        args.push_back(L"/EHsc");
        args.push_back(L"/LD");
        args.push_back(L"/DREALSCRIPT_AOT_BUILD_SHARED_MODULE=1");
        args.push_back(L"/I" + options.includeDirectory.wstring());
        for (const auto& argument : options.compilerArguments) {
            args.push_back(utf8ToWide(argument));
        }
        args.push_back(source.wstring());
        args.push_back(options.supportLibrary.wstring());
        args.push_back(L"/Fe:" + library.wstring());
        args.push_back(
            L"/Fo:" + (library.parent_path() / "realscript_aot_generated.obj").wstring());
    } else {
        args.push_back(L"-std=c++17");
        args.push_back(L"-shared");
        args.push_back(L"-fPIC");
        args.push_back(L"-pthread");
        args.push_back(L"-DREALSCRIPT_AOT_BUILD_SHARED_MODULE=1");
        args.push_back(L"-I");
        args.push_back(options.includeDirectory.wstring());
        for (const auto& argument : options.compilerArguments) {
            args.push_back(utf8ToWide(argument));
        }
        args.push_back(source.wstring());
        args.push_back(options.supportLibrary.wstring());
        args.push_back(L"-o");
        args.push_back(library.wstring());
    }

    std::wstring commandLine;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0u) commandLine.push_back(L' ');
        commandLine += quoteWindowsCommandLineArg(args[i]);
    }

    std::vector<wchar_t> envBlock;
    const bool hasEnvBlock = resolveMsvcEnvironmentBlock(options.compiler, envBlock);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    HANDLE logHandle = CreateFileW(
        log.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (logHandle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');
    const std::wstring compilerApp = options.compiler.wstring();
    const bool launched = startWindowsProcessRedirected(
        compilerApp.c_str(),
        mutableCmd,
        logHandle,
        hasEnvBlock ? envBlock.data() : nullptr,
        hasEnvBlock ? CREATE_UNICODE_ENVIRONMENT : 0,
        pi);
    CloseHandle(logHandle);
    if (!launched) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
#else
    std::vector<std::string> args;
    args.push_back(options.compiler.string());
    if (looksLikeMsvc(options.compiler)) {
        args.emplace_back("/nologo");
        args.emplace_back("/std:c++17");
        args.emplace_back("/EHsc");
        args.emplace_back("/LD");
        args.emplace_back("/DREALSCRIPT_AOT_BUILD_SHARED_MODULE=1");
        args.push_back("/I" + options.includeDirectory.string());
        for (const auto& argument : options.compilerArguments) {
            args.push_back(argument);
        }
        args.push_back(source.string());
        args.push_back(options.supportLibrary.string());
        args.push_back("/Fe:" + library.string());
        args.push_back(
            "/Fo:" + (library.parent_path() / "realscript_aot_generated.obj").string());
    } else {
        args.emplace_back("-std=c++17");
        args.emplace_back("-shared");
        args.emplace_back("-fPIC");
        args.emplace_back("-pthread");
        args.emplace_back("-DREALSCRIPT_AOT_BUILD_SHARED_MODULE=1");
        args.emplace_back("-I");
        args.push_back(options.includeDirectory.string());
        for (const auto& argument : options.compilerArguments) {
            args.push_back(argument);
        }
        args.push_back(source.string());
        args.push_back(options.supportLibrary.string());
        args.emplace_back("-o");
        args.push_back(library.string());
    }

    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        const int devNullFd = open("/dev/null", O_RDONLY);
        if (devNullFd >= 0) {
            (void)dup2(devNullFd, STDIN_FILENO);
            if (devNullFd > STDERR_FILENO) close(devNullFd);
        } else {
            (void)close(STDIN_FILENO);
        }
        const int logFd = open(
            log.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC,
            0644);
        if (logFd < 0) {
            _exit(126);
        }
        (void)dup2(logFd, STDOUT_FILENO);
        (void)dup2(logFd, STDERR_FILENO);
        if (logFd > STDERR_FILENO) close(logFd);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1u);
        for (auto& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        const char* errText = std::strerror(errno);
        if (errText) {
            (void)write(STDERR_FILENO, "execvp failed: ", 15);
            (void)write(STDERR_FILENO, errText, std::strlen(errText));
            (void)write(STDERR_FILENO, "\n", 1);
        }
        _exit(127);
    }

    int waitStatus = 0;
    while (waitpid(pid, &waitStatus, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(waitStatus)) {
        return WEXITSTATUS(waitStatus);
    }
    return -1;
#endif
}

bool isNoExecMountOrLoaderError(
    const std::filesystem::path& libraryPath,
    std::string_view loaderError) noexcept {
#if !defined(_WIN32)
    struct statvfs vfs{};
    const auto parentDir = libraryPath.parent_path();
    const auto checkDir = parentDir.empty() ? std::filesystem::path(".") : parentDir;
    if (statvfs(checkDir.c_str(), &vfs) == 0 ||
        statvfs(libraryPath.c_str(), &vfs) == 0) {
        if ((vfs.f_flag & ST_NOEXEC) != 0UL) {
            return true;
        }
    }
#else
    (void)libraryPath;
#endif
    return loaderError.find("failed to map segment from shared object") != std::string_view::npos ||
        loaderError.find("Operation not permitted") != std::string_view::npos ||
        loaderError.find("Permission denied") != std::string_view::npos;
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
    std::error_code compilerEc;
    std::error_code includeEc;
    std::error_code supportEc;
    return !options.compiler.empty() &&
        std::filesystem::is_regular_file(options.compiler, compilerEc) &&
        !options.includeDirectory.empty() &&
        std::filesystem::is_directory(options.includeDirectory, includeEc) &&
        !options.supportLibrary.empty() &&
        std::filesystem::is_regular_file(options.supportLibrary, supportEc);
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
    std::error_code fileError;
    std::filesystem::create_directories(directory, fileError);
    if (fileError) {
        result.error = "failed to create JIT output directory '" + directory.string() +
            "': " + fileError.message();
        return result;
    }
    if (!ensureFileContent(header, generated.header, result.error) ||
        !ensureFileContent(source, generated.source, result.error) ||
        !ensureFileContent(manifest, generated.manifest, result.error)) {
        return result;
    }

    if (!options.reuseCachedLibrary ||
        !std::filesystem::is_regular_file(library, fileError)) {
        const auto status = invokeCompilerSubprocess(options, source, library, log);
        result.compilerOutput = readFile(log);
        fileError.clear();
        if (status != 0 || !std::filesystem::is_regular_file(library, fileError)) {
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
        const std::string loaderErr = lastLibraryError();
        result.error = "cannot load JIT library '" + library.string() +
            "': " + loaderErr;
        if (isNoExecMountOrLoaderError(library, loaderErr)) {
            result.error += " (ToolchainOptions::outputDirectory '" +
                options.outputDirectory.string() +
                "' appears to reside on a 'noexec' mount or disallow executable page mappings; "
                "configure outputDirectory on an executable filesystem)";
        }
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

} // namespace realscript::jit
