#include "realscript/game/GameProductization.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& item : diagnostics.items()) {
        if (!result.empty()) result.push_back('\n');
        result += item.code + ": " + item.message;
    }
    return result;
}

struct Character {
    int health = 100;
    int readHealth() const { return health; }
};

realscript::game::GameApi makeApi() {
    realscript::game::GameApi api;
    api.type<Character>("Engine.Game", "Character")
        .property<&Character::readHealth>("Health");
    require(api.function(
        "Engine.Game", "Clamp",
        [](int value) { return value < 0 ? 0 : value; }),
        "failed to register host function");
    require(api.valid(), "host API is invalid");
    return api;
}

const char* source = R"(
module Product.State;
import Engine.Game;

class PersistentState
{
    int count;
    bool enabled;
    string label;

    int Add(int value)
    {
        count = count + value;
        return count;
    }
}

class NativeHolder
{
    Character owner;

    void OnCreate(Character value)
    {
        owner = value;
    }
}
)";

void testBytecodePackageLoadingAndIdentity() {
    auto api = makeApi();
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"state.rs", source}});
    require(compiled.succeeded(),
        "compile failed:\n" + diagnosticsText(compiled.diagnostics));

    std::vector<std::vector<std::uint8_t>> encoded;
    for (const auto& module : compiled.modules) {
        encoded.push_back(realscript::bytecode::encodeModule(module));
    }

    realscript::game::GameProgramLoader loader(api);
    auto loaded = loader.loadBytecodeModules(encoded);
    require(loaded.succeeded(),
        "bytecode package load failed:\n" + diagnosticsText(loaded.diagnostics));
    require(loaded.package.programContentHash ==
            realscript::game::stableProgramContentHash(compiled.modules),
        "program content hash changed across bytecode loading");
    require(loaded.package.hostApiHash ==
            realscript::game::stableGameApiHash(api),
        "host API hash changed across bytecode loading");
    require(loaded.package.createRuntime().program()->moduleCount() ==
            compiled.modules.size(),
        "loaded runtime module count is incorrect");

    auto runtime = loaded.package.createRuntime();
    realscript::runtime::RuntimeError error;
    auto state = runtime.createObject(
        "Product.State::PersistentState", error);
    require(state.has_value(),
        "loaded bytecode type could not be instantiated: " + error.message);
    const auto method = runtime.findMethod(state->type(), "Add", 1);
    require(method.has_value() && method->instance(),
        "loaded bytecode instance method reflection is unavailable");
    const auto invoked = runtime.invoke(
        *state, *method, {std::int64_t{7}});
    require(invoked.succeeded &&
            std::get<std::int64_t>(invoked.value) == 7,
        "loaded bytecode instance method invocation failed");
}

void testRestrictedScriptStateRoundTrip() {
    auto api = makeApi();
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"state.rs", source}});
    require(compiled.succeeded(), "state fixture compile failed");

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::runtime::RuntimeError error;
    auto original = runtime.createObject(
        "Product.State::PersistentState", error);
    require(original.has_value(), "failed to create state object: " + error.message);
    require(runtime.setMember(*original, "count", std::int64_t{42}, error),
        "failed to set count: " + error.message);
    require(runtime.setMember(*original, "enabled", true, error),
        "failed to set enabled: " + error.message);
    require(runtime.setMember(*original, "label", std::string{"alpha"}, error),
        "failed to set label: " + error.message);

    auto snapshot = realscript::game::snapshotScriptObject(
        runtime, *original, error);
    require(snapshot.has_value(), "snapshot failed: " + error.message);
    const auto expectedHash = snapshot->canonicalHash();
    const auto bytes = realscript::game::encodeScriptObjectState(
        *snapshot, error);
    require(!bytes.empty(), "state encoding failed: " + error.message);

    auto decoded = realscript::game::decodeScriptObjectState(bytes, error);
    require(decoded.has_value(), "state decoding failed: " + error.message);
    require(decoded->canonicalHash() == expectedHash,
        "state hash changed after encoding");

    auto restored = runtime.createObject(
        "Product.State::PersistentState", error);
    require(restored.has_value(), "failed to create restore target");
    require(realscript::game::restoreScriptObject(
        runtime, *restored, *decoded, error),
        "state restore failed: " + error.message);

    const auto count = runtime.getMember(*restored, "count", error);
    const auto enabled = runtime.getMember(*restored, "enabled", error);
    const auto label = runtime.getMember(*restored, "label", error);
    require(count && std::get<std::int64_t>(*count) == 42,
        "restored count is incorrect");
    require(enabled && std::get<bool>(*enabled),
        "restored enabled flag is incorrect");
    require(label && std::get<std::string>(*label) == "alpha",
        "restored label is incorrect");
}

void testNativeHandlesAreNotPersistentState() {
    auto api = makeApi();
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"state.rs", source}});
    require(compiled.succeeded(), "native fixture compile failed");

    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::runtime::RuntimeError error;
    auto wrapper = runtime.wrapNative(
        api.canonicalTypeName<Character>(),
        std::make_shared<Character>(), error, "native-state-test");
    require(wrapper.has_value(), "failed to create native wrapper");

    auto snapshot = realscript::game::snapshotScriptObject(
        runtime, *wrapper, error);
    require(!snapshot.has_value() &&
            error.code == realscript::runtime::ErrorCode::DeterminismViolation,
        "native handle was accepted as persistent script state");
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

    run("bytecode package loading and identity",
        testBytecodePackageLoadingAndIdentity);
    run("restricted script state round trip",
        testRestrictedScriptStateRoundTrip);
    run("native handles rejected from persistent state",
        testNativeHandlesAreNotPersistentState);
    return failures == 0 ? 0 : 1;
}
