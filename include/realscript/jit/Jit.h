#pragma once

#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/mir/Mir.h"
#include "realscript/optimization/Optimizer.h"

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

} // namespace realscript::jit
