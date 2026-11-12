#pragma once

#include <cstdint>

namespace realscript {

inline constexpr std::uint32_t kVersionMajor = 0;
inline constexpr std::uint32_t kVersionMinor = 98;
inline constexpr std::uint32_t kVersionPatch = 0;

// Bump when source/runtime semantics visible to an embedding host change.
inline constexpr std::uint32_t kSdkCompatibilityVersion = 1;

// Bump when the productized Game SDK package/state contracts change.
inline constexpr std::uint32_t kGameSdkPackageVersion = 1;
inline constexpr std::uint32_t kScriptObjectStateVersion = 1;

inline constexpr const char* kVersionString = "0.98.0";

// P3 Stage 1: Binary ABI, Bytecode (.rsbc), and Snapshot (RSGS) Governance Constants.
inline constexpr std::uint32_t kAbiContractSchemaVersion = 1;

inline constexpr std::uint16_t kBytecodeMinSupportedMajor = 0;
inline constexpr std::uint16_t kBytecodeMinSupportedMinor = 6;
inline constexpr std::uint16_t kBytecodeMaxSupportedMajor = 0;
inline constexpr std::uint16_t kBytecodeMaxSupportedMinor = 9;
inline constexpr std::uint16_t kBytecodeDefaultMajor = 0;
inline constexpr std::uint16_t kBytecodeDefaultMinor = 7;

// Matches GameplayStateCodec ("RSGS") binary format version.
inline constexpr std::uint32_t kRsgsStateEncodingVersion = 1;

// Matches C11 NativeModuleAbi / AOT runtime discovery contract.
inline constexpr std::uint32_t kNativeModuleAbiMajor = 2;
inline constexpr std::uint32_t kNativeModuleAbiMinor = 0;

} // namespace realscript
