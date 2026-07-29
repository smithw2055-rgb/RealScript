#pragma once

#include "realscript/Version.h"
#include "realscript/game/GameScripting.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace realscript::game {

struct GameProgramPackage {
    std::shared_ptr<const runtime::ProgramImage> program;
    std::shared_ptr<const runtime::BindingRegistry> bindings;
    std::shared_ptr<runtime::ManagedHeap> heap;
    std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles;
    std::uint64_t programContentHash = 0;
    std::uint64_t hostApiHash = 0;

    [[nodiscard]] bool valid() const noexcept {
        return program != nullptr && bindings != nullptr && heap != nullptr &&
            nativeHandles != nullptr && programContentHash != 0 && hostApiHash != 0;
    }

    [[nodiscard]] ScriptRuntime createRuntime() const {
        return ScriptRuntime(program, bindings, heap, nativeHandles);
    }
};

struct GameProgramLoadResult {
    GameProgramPackage package;
    std::vector<bytecode::Module> modules;
    diagnostics::DiagnosticBag diagnostics;

    [[nodiscard]] bool succeeded() const noexcept {
        return package.valid() && !diagnostics.hasErrors();
    }
};

class GameProgramLoader {
public:
    explicit GameProgramLoader(const GameApi& api) : api_(api) {}

    [[nodiscard]] GameProgramLoadResult loadBytecodeModules(
        const std::vector<std::vector<std::uint8_t>>& encodedModules) const;

private:
    const GameApi& api_;
};

[[nodiscard]] std::uint64_t stableProgramContentHash(
    const std::vector<bytecode::Module>& modules);
[[nodiscard]] std::uint64_t stableGameApiHash(const GameApi& api);

struct ScriptStatePolicy {
    bool allowStrings = true;
    bool allowDoubles = false;
    std::size_t maximumFields = 4096;
    std::size_t maximumStructDepth = 32;
    std::size_t maximumEncodedBytes = 16u * 1024u * 1024u;
};

struct ScriptFieldState {
    std::string name;
    runtime::Value value;
};

struct ScriptObjectState {
    std::uint32_t version = kScriptObjectStateVersion;
    std::string canonicalTypeName;
    std::vector<ScriptFieldState> fields;

    [[nodiscard]] std::uint64_t canonicalHash() const noexcept;
};

[[nodiscard]] std::optional<ScriptObjectState> snapshotScriptObject(
    const ScriptRuntime& runtime,
    const ScriptObject& object,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {});

bool restoreScriptObject(
    const ScriptRuntime& runtime,
    ScriptObject& object,
    const ScriptObjectState& state,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {});

[[nodiscard]] std::vector<std::uint8_t> encodeScriptObjectState(
    const ScriptObjectState& state,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {});

[[nodiscard]] std::optional<ScriptObjectState> decodeScriptObjectState(
    const std::vector<std::uint8_t>& bytes,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {});

} // namespace realscript::game
