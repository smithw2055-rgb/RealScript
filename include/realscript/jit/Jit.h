#pragma once

#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/mir/Mir.h"
#include "realscript/optimization/Optimizer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace realscript::jit {

struct ToolchainOptions {
    std::filesystem::path compiler;
    std::filesystem::path includeDirectory;
    std::filesystem::path supportLibrary;
    std::filesystem::path outputDirectory;
    std::vector<std::string> compilerArguments;
    optimization::Options optimization{
        optimization::Level::Aggressive,
        4,
        true,
    };
    aot::GenerationOptions generation;
    bool reuseCachedLibrary = true;
    bool keepGeneratedSources = true;
};

class Module {
public:
    Module();
    ~Module();
    Module(Module&&) noexcept;
    Module& operator=(Module&&) noexcept;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t contentHash() const noexcept;
    [[nodiscard]] const std::filesystem::path& libraryPath() const noexcept;
    [[nodiscard]] const aot::ProgramDescriptor* descriptor() const noexcept;

    void setBindings(std::shared_ptr<const runtime::BindingRegistry> bindings);
    void setHeap(std::shared_ptr<runtime::ManagedHeap> heap);
    [[nodiscard]] runtime::ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;

private:
    friend class ToolchainJit;
    struct Impl;
    explicit Module(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct CompileResult {
    std::shared_ptr<Module> module;
    optimization::Statistics optimizationStatistics;
    std::string compilerOutput;
    std::string error;
    bool cacheHit = false;

    [[nodiscard]] bool succeeded() const noexcept {
        return module && module->valid() && error.empty();
    }
};

class ToolchainJit {
public:
    [[nodiscard]] CompileResult compile(
        std::vector<mir::Module> modules,
        ToolchainOptions options) const;

    [[nodiscard]] static bool available(
        const ToolchainOptions& options) noexcept;
};

// ============================================================================
// P2-A: Zero-Dependency In-Memory W^X Copy-and-Patch StencilJit
// ============================================================================

class ExecutableCodeArena {
public:
    explicit ExecutableCodeArena(std::size_t defaultBlockBytes = 65536);
    ~ExecutableCodeArena();
    ExecutableCodeArena(ExecutableCodeArena&&) noexcept;
    ExecutableCodeArena& operator=(ExecutableCodeArena&&) noexcept;
    ExecutableCodeArena(const ExecutableCodeArena&) = delete;
    ExecutableCodeArena& operator=(const ExecutableCodeArena&) = delete;

    [[nodiscard]] std::uint8_t* emit(
        const std::uint8_t* code,
        std::size_t size,
        std::size_t alignment = 16);

    [[nodiscard]] bool seal() noexcept;
    [[nodiscard]] bool sealed() const noexcept;
    [[nodiscard]] std::size_t allocatedBytes() const noexcept;
    [[nodiscard]] std::size_t mappedBytes() const noexcept;
    [[nodiscard]] std::size_t blockCount() const noexcept;

    [[nodiscard]] static std::size_t systemPageSize() noexcept;
    [[nodiscard]] static bool nativeCodeExecutionSupported() noexcept;

private:
    struct Block {
        std::uint8_t* base = nullptr;
        std::size_t capacity = 0;
        std::size_t used = 0;
        bool executable = false;
        bool mapJit = false;
    };

    void releaseAll() noexcept;

    std::vector<Block> blocks_;
    std::size_t defaultBlockBytes_ = 65536;
    std::size_t allocatedBytes_ = 0;
    std::size_t mappedBytes_ = 0;
    bool sealed_ = false;
};

enum class StencilTier : std::uint8_t {
    InterpreterFallback = 0,
    ThreadedStencils = 1,
    NativeMachineCode = 2,
};

enum class StencilTargetArch : std::uint8_t {
    Host = 0,
    X86_64 = 1,
    Arm64 = 2,
};

struct StencilJitOptions {
    bool enableNativeCodegen = true;
    bool enableThreadedFallback = true;
    std::uint32_t hotnessThreshold = 0;
    std::size_t arenaBlockBytes = 65536;
};

class StencilModule {
public:
    StencilModule();
    ~StencilModule();
    StencilModule(StencilModule&&) noexcept;
    StencilModule& operator=(StencilModule&&) noexcept;
    StencilModule(const StencilModule&) = delete;
    StencilModule& operator=(const StencilModule&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const runtime::ProgramImage* program() const noexcept;
    [[nodiscard]] std::shared_ptr<const runtime::ProgramImage> programSnapshot() const noexcept;

    void setBindings(std::shared_ptr<const runtime::BindingRegistry> bindings);
    void setBindingRegistry(std::shared_ptr<const runtime::BindingRegistry> bindings);
    void setExternalResolver(runtime::ExternalFunction resolver);
    void setHeap(std::shared_ptr<runtime::ManagedHeap> heap);
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> heap() const noexcept;
    void setAsyncRuntime(std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime);
    [[nodiscard]] std::shared_ptr<runtime::AsyncRuntimeContext> asyncRuntime() const noexcept;

    [[nodiscard]] runtime::ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;
    [[nodiscard]] runtime::ExecutionResult invoke(
        const std::string& qualifiedName,
        const std::vector<runtime::Value>& arguments,
        runtime::Limits limits) const;
    [[nodiscard]] runtime::ExecutionResult invoke(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;
    [[nodiscard]] runtime::ExecutionResult invoke(
        semantic::SymbolId symbolId,
        const std::vector<runtime::Value>& arguments,
        runtime::Limits limits) const;

    [[nodiscard]] std::size_t totalFunctionCount() const noexcept;
    [[nodiscard]] std::size_t nativeFunctionCount() const noexcept;
    [[nodiscard]] std::size_t threadedFunctionCount() const noexcept;
    [[nodiscard]] std::size_t fallbackFunctionCount() const noexcept;
    [[nodiscard]] std::size_t machineCodeBytes() const noexcept;
    [[nodiscard]] bool nativeBackendActive() const noexcept;
    [[nodiscard]] StencilTier functionTier(semantic::SymbolId symbolId) const noexcept;
    [[nodiscard]] StencilTier functionTier(const std::string& qualifiedName) const noexcept;
    [[nodiscard]] StencilTargetArch hostTargetArch() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> inspectMachineCode(
        const std::string& qualifiedName) const;
    [[nodiscard]] std::vector<std::uint8_t> inspectMachineCode(
        const std::string& qualifiedName,
        StencilTargetArch targetArch) const;
    [[nodiscard]] runtime::ExecutionResult invokeSimulatedArm64(
        const std::string& qualifiedName,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;

    [[nodiscard]] runtime::ExecutionResult invokeWithBaseFrameOffset(
        const std::string& qualifiedName,
        std::size_t baseFrameOffset,
        bool simulateArm64 = false,
        const std::vector<runtime::Value>& arguments = {},
        runtime::ExecutionOptions options = {}) const;

private:
    friend class StencilJit;
    struct Impl;
    explicit StencilModule(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

struct StencilCompileResult {
    std::shared_ptr<StencilModule> module;
    double compileTimeMicroseconds = 0.0;
    std::size_t totalFunctionCount = 0;
    std::size_t nativeFunctionCount = 0;
    std::size_t threadedFunctionCount = 0;
    std::size_t fallbackFunctionCount = 0;
    std::size_t machineCodeBytes = 0;
    bool nativeBackendActive = false;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept {
        return module && module->valid() && error.empty();
    }
};

class StencilJit {
public:
    [[nodiscard]] StencilCompileResult compile(
        std::shared_ptr<const runtime::ProgramImage> program,
        StencilJitOptions options = {}) const;

    [[nodiscard]] StencilCompileResult compile(
        std::vector<bytecode::Module> modules,
        StencilJitOptions options = {}) const;

    [[nodiscard]] static bool nativeAvailable() noexcept;
};

} // namespace realscript::jit
