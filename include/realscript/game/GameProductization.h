#pragma once

#include "realscript/AbiGovernance.h"
#include "realscript/Version.h"
#include "realscript/game/GameScripting.h"
#include "realscript/game/GameplayScripting.h"
#include "realscript/game/GameplayStateCodec.h"
#include "realscript/text/AssetGuid.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realscript::game {

struct GameProgramPackage {
    std::shared_ptr<const runtime::ProgramImage> program;
    std::shared_ptr<const runtime::BindingRegistry> bindings;
    std::shared_ptr<runtime::ManagedHeap> heap;
    std::shared_ptr<runtime::NativeHandleRegistry> nativeHandles;
    std::uint64_t programContentHash = 0;
    std::uint64_t hostApiHash = 0;
    AbiContractManifest abiManifest = currentAbiContractManifest();

    [[nodiscard]] bool valid() const noexcept {
        return program != nullptr && bindings != nullptr && heap != nullptr &&
            nativeHandles != nullptr && programContentHash != 0 &&
            hostApiHash != 0 && abiManifest.hasValidDigest();
    }

    [[nodiscard]] ScriptRuntime createRuntime() const {
        return ScriptRuntime(program, bindings, heap, nativeHandles);
    }
};

struct GameProgramLoadResult {
    GameProgramPackage package;
    std::vector<bytecode::Module> modules;
    diagnostics::DiagnosticBag diagnostics;
    AbiCompatibilityReport abiCompatibility{};

    [[nodiscard]] bool succeeded() const noexcept {
        return package.valid() && !diagnostics.hasErrors() &&
            abiCompatibility.canLoad();
    }
};

class GameProgramLoader {
public:
    explicit GameProgramLoader(const GameApi& api) : api_(api) {}

    [[nodiscard]] GameProgramLoadResult loadBytecodeModules(
        const std::vector<std::vector<std::uint8_t>>& encodedModules) const;

    [[nodiscard]] GameProgramLoadResult loadBytecodeModules(
        const std::vector<std::vector<std::uint8_t>>& encodedModules,
        const AbiContractManifest& embeddedPackageManifest) const;

private:
    const GameApi& api_;
};

[[nodiscard]] std::uint64_t stableProgramContentHash(
    const std::vector<bytecode::Module>& modules);
[[nodiscard]] std::uint64_t stableGameApiHash(const GameApi& api);

// Package-level ABI Contract Embedding & Preflight Governance Helpers
struct GamePackageContractEnvelope {
    std::uint32_t packageVersion = kGameSdkPackageVersion;
    std::uint64_t programContentHash = 0;
    std::uint64_t hostApiHash = 0;
    std::uint32_t moduleCount = 0;
    AbiContractManifest abiManifest = currentAbiContractManifest();

    [[nodiscard]] std::string toJson() const;
    [[nodiscard]] static std::optional<GamePackageContractEnvelope> fromJson(
        std::string_view jsonText,
        std::string* outError = nullptr);
};

[[nodiscard]] AbiContractManifest buildPackageAbiManifest(
    const std::vector<bytecode::Module>& modules) noexcept;

[[nodiscard]] GamePackageContractEnvelope buildPackageContractEnvelope(
    const GameProgramPackage& package) noexcept;

[[nodiscard]] AbiCompatibilityReport verifyPackageAbiCompatibility(
    const GameProgramPackage& package,
    const AbiContractManifest& hostManifest = currentAbiContractManifest());

[[nodiscard]] AbiCompatibilityReport verifyPackageContractEnvelope(
    const GamePackageContractEnvelope& envelope,
    const GameApi* expectedApi = nullptr,
    std::uint64_t expectedProgramContentHash = 0,
    const AbiContractManifest& hostManifest = currentAbiContractManifest());

[[nodiscard]] AbiCompatibilityReport preflightBytecodePackageHeaders(
    const std::vector<std::vector<std::uint8_t>>& encodedModules,
    const AbiContractManifest& hostManifest = currentAbiContractManifest());

struct ScriptStatePolicy {
    bool allowStrings = true;
    bool allowDoubles = false;
    std::size_t maximumFields = 4096;
    std::size_t maximumStructDepth = 32;
    std::size_t maximumEncodedBytes = 16u * 1024u * 1024u;
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ScriptFieldState {
    std::string name;
    runtime::Value value;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
    ScriptRuntime& runtime,
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

[[nodiscard]] std::string exportScriptObjectJson(
    const ScriptRuntime& runtime,
    const ScriptObject& object,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {},
    const SceneScriptRuntime* scene = nullptr);

bool importScriptObjectJson(
    ScriptRuntime& runtime,
    ScriptObject& object,
    const std::string& jsonText,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {},
    SceneScriptRuntime* scene = nullptr);

[[nodiscard]] std::string exportSceneJson(
    const SceneScriptRuntime& scene,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {});

bool importSceneJson(
    SceneScriptRuntime& scene,
    const std::string& jsonText,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {});

struct RollbackFrameSnapshot {
    GameTick tick = 0;
    std::vector<std::uint8_t> hostStateBytes;
    std::string sceneJson;
    std::uint64_t frameHash = 0;
};

[[nodiscard]] std::optional<RollbackFrameSnapshot> captureRollbackFrame(
    const SceneScriptRuntime& scene,
    const GameplayHost& host,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {});

bool restoreRollbackFrame(
    SceneScriptRuntime& scene,
    GameplayHost& host,
    const RollbackFrameSnapshot& snapshot,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy = {});

// ============================================================================
// eff_3: Deterministic 128-bit / Standard UUID AssetGuid & Git-Friendly Text Serialization
// ============================================================================

using AssetGuid = ::realscript::text::AssetGuid;

class AssetGuidRegistry {
public:
    AssetGuidRegistry() = default;

    AssetGuid registerType(
        std::string_view canonicalTypeName,
        std::optional<AssetGuid> explicitGuid = std::nullopt);
    bool rebindType(AssetGuid guid, std::string_view newCanonicalTypeName);
    AssetGuid registerRenamedType(
        std::string_view oldCanonicalTypeName,
        std::string_view newCanonicalTypeName);

    AssetGuid registerAssetPath(
        std::string_view assetPath,
        std::optional<AssetGuid> explicitGuid = std::nullopt);
    bool rebindAssetPath(AssetGuid guid, std::string_view newAssetPath);
    AssetGuid registerRenamedAssetPath(
        std::string_view oldAssetPath,
        std::string_view newAssetPath);

    void populateFromProgram(const runtime::ProgramImage& program);
    void populateFromProgram(const GameProgram& program);

    [[nodiscard]] std::optional<AssetGuid> findGuidForType(
        std::string_view canonicalTypeName) const;
    [[nodiscard]] AssetGuid guidForTypeOrDeterministic(
        std::string_view canonicalTypeName) const;
    [[nodiscard]] std::optional<std::string> resolveTypeName(AssetGuid guid) const;

    [[nodiscard]] std::optional<AssetGuid> findGuidForAssetPath(
        std::string_view assetPath) const;
    [[nodiscard]] AssetGuid guidForAssetPathOrDeterministic(
        std::string_view assetPath) const;
    [[nodiscard]] std::optional<std::string> resolveAssetPath(AssetGuid guid) const;

    [[nodiscard]] std::optional<AssetGuid> findGuid(std::string_view key) const;
    [[nodiscard]] std::optional<std::string> resolveIdentity(AssetGuid guid) const;

    bool unregisterGuid(AssetGuid guid);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return typeByGuid_.size() + assetPathByGuid_.size();
    }
    [[nodiscard]] std::size_t registeredTypeCount() const noexcept {
        return typeByGuid_.size();
    }
    [[nodiscard]] std::size_t registeredAssetCount() const noexcept {
        return assetPathByGuid_.size();
    }

    [[nodiscard]] std::string serializeRegistryText() const;
    bool deserializeRegistryText(
        std::string_view text,
        runtime::RuntimeError& error);

private:
    struct GuidHasher {
        std::size_t operator()(AssetGuid g) const noexcept {
            std::uint64_t x = g.high ^ (g.low + 0x9e3779b97f4a7c15ULL + (g.high << 6u) + (g.high >> 2u));
            return static_cast<std::size_t>(x);
        }
    };

    std::unordered_map<AssetGuid, std::string, GuidHasher> typeByGuid_;
    std::unordered_map<std::string, AssetGuid> guidByType_;
    std::unordered_map<AssetGuid, std::string, GuidHasher> assetPathByGuid_;
    std::unordered_map<std::string, AssetGuid> guidByAssetPath_;
};

[[nodiscard]] std::string serializeSceneText(
    const SceneScriptRuntime& scene,
    runtime::RuntimeError& error,
    const AssetGuidRegistry* guidRegistry = nullptr,
    ScriptStatePolicy policy = {});

bool deserializeSceneText(
    SceneScriptRuntime& scene,
    const std::string& text,
    runtime::RuntimeError& error,
    const AssetGuidRegistry* guidRegistry = nullptr,
    ScriptStatePolicy policy = {});

[[nodiscard]] std::string serializePrefabText(
    const PrefabNodeTemplate& prefab,
    runtime::RuntimeError& error,
    const AssetGuidRegistry* guidRegistry = nullptr,
    const ScriptRuntime* runtime = nullptr,
    ScriptStatePolicy policy = {});

[[nodiscard]] std::optional<PrefabNodeTemplate> deserializePrefabText(
    const std::string& text,
    runtime::RuntimeError& error,
    const AssetGuidRegistry* guidRegistry = nullptr,
    ScriptRuntime* runtime = nullptr,
    ScriptStatePolicy policy = {});

} // namespace realscript::game
