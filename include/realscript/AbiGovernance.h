#pragma once

#include "realscript/Version.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realscript {

struct BytecodeVersionPoint {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;

    constexpr BytecodeVersionPoint() noexcept = default;
    constexpr BytecodeVersionPoint(std::uint16_t maj, std::uint16_t min) noexcept
        : major(maj), minor(min) {}

    [[nodiscard]] std::string toString() const;

    friend constexpr bool operator==(
        BytecodeVersionPoint a,
        BytecodeVersionPoint b) noexcept {
        return a.major == b.major && a.minor == b.minor;
    }
    friend constexpr bool operator!=(
        BytecodeVersionPoint a,
        BytecodeVersionPoint b) noexcept {
        return !(a == b);
    }
    friend constexpr bool operator<(
        BytecodeVersionPoint a,
        BytecodeVersionPoint b) noexcept {
        return a.major != b.major ? (a.major < b.major) : (a.minor < b.minor);
    }
    friend constexpr bool operator<=(
        BytecodeVersionPoint a,
        BytecodeVersionPoint b) noexcept {
        return !(b < a);
    }
    friend constexpr bool operator>(
        BytecodeVersionPoint a,
        BytecodeVersionPoint b) noexcept {
        return b < a;
    }
    friend constexpr bool operator>=(
        BytecodeVersionPoint a,
        BytecodeVersionPoint b) noexcept {
        return !(a < b);
    }
};

struct NativeModuleAbiContract {
    std::uint32_t abiMajor = kNativeModuleAbiMajor;
    std::uint32_t abiMinor = kNativeModuleAbiMinor;
    std::uint32_t runtimeApiStructSize = 0;
    std::uint32_t functionEntryStructSize = 0;
    std::uint32_t moduleExportsStructSize = 0;

    friend constexpr bool operator==(
        const NativeModuleAbiContract& a,
        const NativeModuleAbiContract& b) noexcept {
        return a.abiMajor == b.abiMajor &&
            a.abiMinor == b.abiMinor &&
            a.runtimeApiStructSize == b.runtimeApiStructSize &&
            a.functionEntryStructSize == b.functionEntryStructSize &&
            a.moduleExportsStructSize == b.moduleExportsStructSize;
    }
    friend constexpr bool operator!=(
        const NativeModuleAbiContract& a,
        const NativeModuleAbiContract& b) noexcept {
        return !(a == b);
    }
};

struct AbiContractManifest {
    std::uint32_t schemaVersion = kAbiContractSchemaVersion;
    std::uint32_t engineVersionMajor = kVersionMajor;
    std::uint32_t engineVersionMinor = kVersionMinor;
    std::uint32_t engineVersionPatch = kVersionPatch;

    std::uint32_t sdkCompatibilityVersion = kSdkCompatibilityVersion;
    std::uint32_t gameSdkPackageVersion = kGameSdkPackageVersion;
    std::uint32_t scriptObjectStateVersion = kScriptObjectStateVersion;

    BytecodeVersionPoint bytecodeMinVersion{
        kBytecodeMinSupportedMajor, kBytecodeMinSupportedMinor};
    BytecodeVersionPoint bytecodeMaxVersion{
        kBytecodeMaxSupportedMajor, kBytecodeMaxSupportedMinor};
    BytecodeVersionPoint bytecodeTargetVersion{
        kBytecodeDefaultMajor, kBytecodeDefaultMinor};

    std::uint32_t rsgsStateVersion = kRsgsStateEncodingVersion;

    std::uint32_t runtimeValueSize = 0;
    std::uint32_t runtimeValueAlign = 0;
    std::uint32_t bytecodeInstructionSize = 0;
    std::uint32_t bytecodeInstructionAlign = 0;
    std::uint32_t compactInstructionSize = 0;

    NativeModuleAbiContract nativeModuleAbi{};
    std::uint64_t layoutDigest = 0;

    [[nodiscard]] std::uint64_t computeLayoutDigest() const noexcept;
    void refreshLayoutDigest() noexcept { layoutDigest = computeLayoutDigest(); }
    [[nodiscard]] bool hasValidDigest() const noexcept {
        return layoutDigest != 0 && layoutDigest == computeLayoutDigest();
    }

    [[nodiscard]] std::string toJson() const;
    [[nodiscard]] static std::optional<AbiContractManifest> fromJson(
        std::string_view jsonText,
        std::string* outError = nullptr);

    friend constexpr bool operator==(
        const AbiContractManifest& a,
        const AbiContractManifest& b) noexcept {
        return a.schemaVersion == b.schemaVersion &&
            a.engineVersionMajor == b.engineVersionMajor &&
            a.engineVersionMinor == b.engineVersionMinor &&
            a.engineVersionPatch == b.engineVersionPatch &&
            a.sdkCompatibilityVersion == b.sdkCompatibilityVersion &&
            a.gameSdkPackageVersion == b.gameSdkPackageVersion &&
            a.scriptObjectStateVersion == b.scriptObjectStateVersion &&
            a.bytecodeMinVersion == b.bytecodeMinVersion &&
            a.bytecodeMaxVersion == b.bytecodeMaxVersion &&
            a.bytecodeTargetVersion == b.bytecodeTargetVersion &&
            a.rsgsStateVersion == b.rsgsStateVersion &&
            a.runtimeValueSize == b.runtimeValueSize &&
            a.runtimeValueAlign == b.runtimeValueAlign &&
            a.bytecodeInstructionSize == b.bytecodeInstructionSize &&
            a.bytecodeInstructionAlign == b.bytecodeInstructionAlign &&
            a.compactInstructionSize == b.compactInstructionSize &&
            a.nativeModuleAbi == b.nativeModuleAbi &&
            a.layoutDigest == b.layoutDigest;
    }
    friend constexpr bool operator!=(
        const AbiContractManifest& a,
        const AbiContractManifest& b) noexcept {
        return !(a == b);
    }
};

enum class AbiCompatibilityStatus : std::uint8_t {
    Compatible = 0,
    CompatibleWithMigration = 1,
    Incompatible = 2,
};

enum class AbiIssueCode : std::uint16_t {
    None = 0,
    LayoutDigestMismatch,
    SchemaVersionMismatch,
    EngineMajorMismatch,
    EngineMinorNewerThanHost,
    EngineMinorOlderMigration,
    SdkCompatibilityMismatch,
    GameSdkPackageNewerThanHost,
    GameSdkPackageOlderMigration,
    ScriptObjectStateNewerThanHost,
    ScriptObjectStateMigrationRequired,
    RsgsStateNewerThanHost,
    RsgsStateMigrationRequired,
    BytecodeVersionRangeInvalid,
    BytecodeVersionRangeDisjoint,
    BytecodeTargetVersionUnsupported,
    BytecodeLegacyMetadataMigration,
    BytecodeHeaderTruncated,
    BytecodeInvalidMagic,
    BytecodeSectionCountMismatch,
    RuntimeValueLayoutMismatch,
    BytecodeInstructionLayoutMismatch,
    CompactInstructionLayoutMismatch,
    NativeModuleAbiMajorMismatch,
    NativeModuleAbiMinorNewerThanHost,
    NativeModuleAbiMinorBackwardCompatible,
    NativeModuleAbiStructSizeMismatch,
};

struct AbiIssue {
    AbiIssueCode code = AbiIssueCode::None;
    AbiCompatibilityStatus impact = AbiCompatibilityStatus::Compatible;
    std::string diagnosticCode;
    std::string field;
    std::string message;
};

struct AbiCompatibilityReport {
    AbiCompatibilityStatus status = AbiCompatibilityStatus::Compatible;
    std::vector<AbiIssue> issues;

    [[nodiscard]] bool compatible() const noexcept {
        return status == AbiCompatibilityStatus::Compatible;
    }
    [[nodiscard]] bool canLoad() const noexcept {
        return status == AbiCompatibilityStatus::Compatible ||
            status == AbiCompatibilityStatus::CompatibleWithMigration;
    }
    [[nodiscard]] bool requiresMigration() const noexcept {
        return status == AbiCompatibilityStatus::CompatibleWithMigration;
    }
    [[nodiscard]] bool hasIssue(AbiIssueCode expectedCode) const noexcept;
    [[nodiscard]] std::string summary() const;
};

struct BytecodeHeaderCompatibilityReport {
    AbiCompatibilityStatus status = AbiCompatibilityStatus::Incompatible;
    bool validMagic = false;
    bool truncated = false;
    BytecodeVersionPoint version{};
    std::uint32_t headerFlags = 0;
    std::uint32_t sectionCount = 0;
    std::uint32_t expectedSectionCount = 0;
    bool hasObjectModelSection = false;
    bool hasSequenceResultMetadata = false;
    bool hasExceptionMetadata = false;
    std::vector<AbiIssue> issues;

    [[nodiscard]] bool compatible() const noexcept {
        return status == AbiCompatibilityStatus::Compatible;
    }
    [[nodiscard]] bool canLoad() const noexcept {
        return status == AbiCompatibilityStatus::Compatible ||
            status == AbiCompatibilityStatus::CompatibleWithMigration;
    }
    [[nodiscard]] bool requiresMigration() const noexcept {
        return status == AbiCompatibilityStatus::CompatibleWithMigration;
    }
    [[nodiscard]] bool hasIssue(AbiIssueCode expectedCode) const noexcept;
};

[[nodiscard]] const char* abiCompatibilityStatusName(
    AbiCompatibilityStatus status) noexcept;
[[nodiscard]] const char* abiIssueCodeName(AbiIssueCode code) noexcept;

[[nodiscard]] AbiContractManifest currentAbiContractManifest() noexcept;

[[nodiscard]] AbiCompatibilityReport verifyAbiCompatibility(
    const AbiContractManifest& host,
    const AbiContractManifest& target);

[[nodiscard]] BytecodeHeaderCompatibilityReport inspectBytecodeHeaderCompatibility(
    const std::uint8_t* data,
    std::size_t size,
    const AbiContractManifest& host = currentAbiContractManifest());

[[nodiscard]] BytecodeHeaderCompatibilityReport inspectBytecodeHeaderCompatibility(
    const std::vector<std::uint8_t>& bytes,
    const AbiContractManifest& host = currentAbiContractManifest());

} // namespace realscript
