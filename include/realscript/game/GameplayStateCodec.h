#pragma once

#include "realscript/game/GameplayScripting.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace realscript::game {

constexpr std::uint32_t kGameplayStateCodecVersion = 1;

enum class GameplayStateErrorCode {
    None,
    InvalidState,
    InvalidMagic,
    UnsupportedVersion,
    TruncatedInput,
    MalformedInput,
    LimitExceeded,
    HashMismatch,
};

struct GameplayStateError {
    GameplayStateErrorCode code = GameplayStateErrorCode::None;
    std::string message;

    [[nodiscard]] bool failed() const noexcept {
        return code != GameplayStateErrorCode::None;
    }
};

struct GameplayStateCodecLimits {
    std::size_t maximumEncodedBytes = 16u * 1024u * 1024u;
    std::size_t maximumEntities = 1'000'000;
    std::size_t maximumTasks = 1'000'000;
    std::size_t maximumEvents = 1'000'000;
    std::size_t maximumCalls = 1'000'000;
    std::size_t maximumSubscriptions = 1'000'000;
    std::size_t maximumSequences = 1'000'000;
    std::size_t maximumArgumentsPerCall = 4096;
    std::size_t maximumStringBytes = 1u * 1024u * 1024u;
};

[[nodiscard]] std::vector<std::uint8_t> encodeGameplayHostState(
    const GameplayHost::State& state,
    GameplayStateError& error,
    GameplayStateCodecLimits limits = {});

[[nodiscard]] std::vector<std::uint8_t> encodeGameplayHostState(
    const GameplayHost& host,
    GameplayStateError& error,
    GameplayStateCodecLimits limits = {});

[[nodiscard]] std::optional<GameplayHost::State> decodeGameplayHostState(
    const std::vector<std::uint8_t>& bytes,
    GameplayStateError& error,
    GameplayStateCodecLimits limits = {});

bool restoreGameplayHostState(
    GameplayHost& host,
    const std::vector<std::uint8_t>& bytes,
    GameplayStateError& error,
    GameplayStateCodecLimits limits = {});

[[nodiscard]] const char* gameplayStateErrorCodeName(
    GameplayStateErrorCode code) noexcept;

} // namespace realscript::game
