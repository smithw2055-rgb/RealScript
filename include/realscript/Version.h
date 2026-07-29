#pragma once

#include <cstdint>

namespace realscript {

inline constexpr std::uint32_t kVersionMajor = 0;
inline constexpr std::uint32_t kVersionMinor = 1;
inline constexpr std::uint32_t kVersionPatch = 0;

// Bump when source/runtime semantics visible to an embedding host change.
inline constexpr std::uint32_t kSdkCompatibilityVersion = 1;

// Bump when the productized Game SDK package/state contracts change.
inline constexpr std::uint32_t kGameSdkPackageVersion = 1;
inline constexpr std::uint32_t kScriptObjectStateVersion = 1;

inline constexpr const char* kVersionString = "0.1.0";

} // namespace realscript
