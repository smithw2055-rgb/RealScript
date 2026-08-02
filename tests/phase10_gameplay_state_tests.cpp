#include "realscript/game/GameplayStateCodec.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace realscript::game;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

GameplayHost createFixture() {
    GameplayHost host(30, 99, 13);
    const auto entity = host.core().entities().create();
    require(entity.valid(), "fixture entity allocation failed");
    require(host.scheduleCall(
        entity.packed(),
        "OnTimer",
        4,
        {std::int32_t{7}, std::string{"payload"}},
        3,
        5) != 0,
        "fixture timer scheduling failed");
    require(host.subscribe(
        "unit.spawned", entity.packed(), "OnSpawned", -5) != 0,
        "fixture subscription failed");
    require(host.publish(
        "unit.spawned",
        {GameplayValue{entity}, GameplayValue{2.5}},
        2) != 0,
        "fixture event publication failed");
    require(host.startSequence(
        entity.packed(),
        {{1, "OnWindup", {}}, {2, "OnImpact", {std::int64_t{12}}}},
        2) != 0,
        "fixture sequence scheduling failed");
    const auto clock = host.core().clock().accumulate(0.01, 8);
    require(clock.steps == 0, "fixture clock unexpectedly advanced");
    return host;
}

void testStateCodecRoundTrip() {
    auto host = createFixture();
    GameplayStateError error;
    const auto encoded = encodeGameplayHostState(host, error);
    require(!encoded.empty() && !error.failed(),
        "state encoding failed: " + error.message);

    const auto decoded = decodeGameplayHostState(encoded, error);
    require(decoded.has_value() && !error.failed(),
        "state decoding failed: " + error.message);

    GameplayHost restored;
    require(restored.restore(*decoded), "decoded state restore failed");
    require(restored.stableHash() == host.stableHash(),
        "state hash changed across codec round trip");

    GameplayHost restoredDirect;
    require(restoreGameplayHostState(restoredDirect, encoded, error),
        "direct state restore failed: " + error.message);
    require(restoredDirect.stableHash() == host.stableHash(),
        "direct state restore produced a different hash");
}

void testCorruptionAndLimitsAreRejected() {
    auto host = createFixture();
    GameplayStateError error;
    auto encoded = encodeGameplayHostState(host, error);
    require(!encoded.empty(), "fixture encoding failed");

    auto corrupted = encoded;
    corrupted.back() ^= 0x5a;
    const auto decoded = decodeGameplayHostState(corrupted, error);
    require(!decoded.has_value(), "corrupted state was accepted");
    require(error.code == GameplayStateErrorCode::HashMismatch ||
            error.code == GameplayStateErrorCode::InvalidState ||
            error.code == GameplayStateErrorCode::MalformedInput,
        "corrupted state produced an unexpected error");

    GameplayStateCodecLimits limits;
    limits.maximumEncodedBytes = 32;
    const auto limited = encodeGameplayHostState(host, error, limits);
    require(limited.empty() && error.code == GameplayStateErrorCode::LimitExceeded,
        "encoded byte limit was not enforced");

    auto badMagic = encoded;
    badMagic[0] = 'X';
    require(!decodeGameplayHostState(badMagic, error).has_value() &&
            error.code == GameplayStateErrorCode::InvalidMagic,
        "invalid state magic was not rejected");
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };

    run("gameplay state codec round trip", testStateCodecRoundTrip);
    run("gameplay state corruption and limits", testCorruptionAndLimitsAreRejected);
    return failures == 0 ? 0 : 1;
}
