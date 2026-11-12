#include "realscript/game/Gameplay.h"
#include "realscript/tooling/Json.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>

namespace {

#if defined(_WIN32)
using TestSocket = SOCKET;
constexpr TestSocket kInvalidTestSocket = INVALID_SOCKET;

void initTestSockets() noexcept {
    WSADATA wsaData{};
    (void)WSAStartup(MAKEWORD(2, 2), &wsaData);
}

void closeTestSocket(TestSocket s) noexcept {
    if (s != kInvalidTestSocket) {
        (void)shutdown(s, SD_BOTH);
        (void)closesocket(s);
    }
}
#else
using TestSocket = int;
constexpr TestSocket kInvalidTestSocket = -1;

void initTestSockets() noexcept {}

void closeTestSocket(TestSocket s) noexcept {
    if (s != kInvalidTestSocket) {
        (void)::shutdown(s, SHUT_RDWR);
        (void)::close(s);
    }
}
#endif

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class ClientSocketBuf final : public std::streambuf {
public:
    explicit ClientSocketBuf(TestSocket fd) noexcept : fd_(fd) {
        setg(inBuf_, inBuf_, inBuf_);
        setp(outBuf_, outBuf_ + sizeof(outBuf_));
    }
    ~ClientSocketBuf() override { (void)sync(); }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        const auto n = ::recv(fd_, inBuf_, static_cast<int>(sizeof(inBuf_)), 0);
        if (n <= 0) return traits_type::eof();
        setg(inBuf_, inBuf_, inBuf_ + n);
        return traits_type::to_int_type(*gptr());
    }
    int_type overflow(int_type ch) override {
        if (flushOut() < 0) return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }
    int sync() override { return flushOut(); }

private:
    int flushOut() noexcept {
        const char* p = pbase();
        int rem = static_cast<int>(pptr() - pbase());
        while (rem > 0) {
            const auto sent = ::send(fd_, p, rem, 0);
            if (sent <= 0) return -1;
            p += sent;
            rem -= static_cast<int>(sent);
        }
        setp(outBuf_, outBuf_ + sizeof(outBuf_));
        return 0;
    }

    TestSocket fd_ = kInvalidTestSocket;
    char inBuf_[4096]{};
    char outBuf_[4096]{};
};

realscript::tooling::Json readNextDapMessage(std::istream& in) {
    std::string body;
    require(realscript::tooling::readProtocolMessage(in, body), "failed to read DAP message from socket");
    std::string err;
    auto parsed = realscript::tooling::Json::parse(body, err);
    require(parsed.has_value(), "failed to parse DAP JSON: " + err);
    return *parsed;
}

realscript::tooling::Json waitForDapEvent(std::istream& in, const std::string& eventName) {
    while (true) {
        auto msg = readNextDapMessage(in);
        const auto* type = msg.find("type");
        if (type && type->stringValue() == "event") {
            const auto* ev = msg.find("event");
            if (ev && ev->stringValue() == eventName) return msg;
        }
    }
}

} // namespace

int main() {
    using namespace realscript;

    game::GameApi api;
    const char* combatSrc = R"RS(
module Game.Combat;

class LootCache {
    public int gold = 500;
}

class BossUnit {
    public int hp = 1000;
    public LootCache retainedLoot = new LootCache();

    public int ApplyHit(int rawDamage) {
        int damage = rawDamage + 10;
        hp = hp - damage;
        return hp;
    }
}
)RS";

    game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"scripts/combat.rscript", combatSrc}});
    require(compiled.succeeded(), "scripts/combat.rscript compile failed");

    game::ScriptRuntime rt(compiled.program);
    game::SceneScriptRuntime scene(rt);
    require(scene.attachComponent(1, "Game.Combat::BossUnit"), "attachComponent BossUnit failed");

    // 1. Start EmbeddedDapServer inside ScriptRuntime on an ephemeral port (port = 0)
    require(rt.startRemoteDebugger(0, "127.0.0.1"), "startRemoteDebugger failed");
    const std::uint16_t port = rt.remoteDebuggerPort();
    require(port > 0, "remoteDebuggerPort() must return a valid bound TCP port");
    std::cout << "[Engine] EmbeddedDapServer listening on 127.0.0.1:" << port << "\n";

    // 2. Connect TCP client simulating VS Code DebugAdapterServer
    initTestSockets();
    TestSocket sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(sock != kInvalidTestSocket, "failed to create client socket");
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    (void)::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
    require(
        ::connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == 0,
        "failed to connect to EmbeddedDapServer TCP port");

    ClientSocketBuf clientBuf(sock);
    std::iostream dapStream(&clientBuf);

    // 3. Send initialize -> setBreakpoints (using full workspace path!) -> attach -> configurationDone
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 1},
        {"type", "request"},
        {"command", "initialize"},
        {"arguments", tooling::Json::Object{}},
    });
    dapStream.flush();
    (void)readNextDapMessage(dapStream); // response
    (void)readNextDapMessage(dapStream); // initialized event

    // Line 14 is `hp = hp - damage;` (after `int damage = rawDamage + 10;`)
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 2},
        {"type", "request"},
        {"command", "setBreakpoints"},
        {"arguments", tooling::Json::Object{
            {"source", tooling::Json::Object{{"path", "/workspace/my_game/scripts/combat.rscript"}}},
            {"breakpoints", tooling::Json::Array{
                tooling::Json::Object{{"line", 14}, {"column", 1}},
            }},
        }},
    });
    dapStream.flush();
    const auto bpResp = readNextDapMessage(dapStream);
    const auto* bps = bpResp.find("body")->find("breakpoints");
    require(
        bps && bps->isArray() && !bps->arrayValue().empty() &&
            bps->arrayValue()[0].find("verified")->boolValue(),
        "setBreakpoints with full workspace path '/workspace/my_game/scripts/combat.rscript' must verify against 'scripts/combat.rscript'");

    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 3},
        {"type", "request"},
        {"command", "attach"},
        {"arguments", tooling::Json::Object{{"stopOnEntry", false}}},
    });
    dapStream.flush();
    const auto attachResp = readNextDapMessage(dapStream);
    require(
        attachResp.find("success") && attachResp.find("success")->boolValue(),
        "DAP 'attach' request must succeed");

    // 4. Run engine frame invocation on a separate thread (simulating Game Engine Main Loop)
    runtime::ExecutionResult frameResult;
    std::thread engineThread([&]() {
        frameResult = scene.invoke(1, "ApplyHit", {std::int64_t{40}});
    });

    // 5. Wait for DAP 'stopped' event over TCP socket when engineThread hits line 9
    const auto stoppedEv = waitForDapEvent(dapStream, "stopped");
    require(
        stoppedEv.find("body")->find("reason")->stringValue() == "breakpoint",
        "Engine thread must stop with reason 'breakpoint'");
    std::cout << "[VS Code Client] Received 'stopped' event over TCP (reason=breakpoint)\n";

    // 6. Inspect stackTrace (verify path alias mapping back to /workspace/my_game/scripts/combat.rscript)
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 4},
        {"type", "request"},
        {"command", "stackTrace"},
        {"arguments", tooling::Json::Object{{"threadId", 1}}},
    });
    dapStream.flush();
    const auto stResp = readNextDapMessage(dapStream);
    const auto* frames = stResp.find("body")->find("stackFrames");
    require(
        frames && !frames->arrayValue().empty() &&
            frames->arrayValue()[0].find("source")->find("path")->stringValue() ==
                "/workspace/my_game/scripts/combat.rscript",
        "stackTrace must resolve source path back to VS Code workspace path '/workspace/my_game/scripts/combat.rscript'");

    // 7. Evaluate Watch expression `damage * 2` (damage = 40 + 10 = 50 -> 100)
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 5},
        {"type", "request"},
        {"command", "evaluate"},
        {"arguments", tooling::Json::Object{
            {"expression", "damage * 2"},
            {"frameId", 1},
        }},
    });
    dapStream.flush();
    const auto evalResp = readNextDapMessage(dapStream);
    require(
        evalResp.find("body")->find("result")->stringValue() == "100",
        "Watch evaluation 'damage * 2' over TCP should return '100'");

    // 8. Mutate live local variable `damage` from 50 to 300 via DAP `setVariable`
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 6},
        {"type", "request"},
        {"command", "setVariable"},
        {"arguments", tooling::Json::Object{
            {"variablesReference", 3}, // frame 1 locals
            {"name", "damage"},
            {"value", "300"},
        }},
    });
    dapStream.flush();
    const auto setVarResp = readNextDapMessage(dapStream);
    require(
        setVarResp.find("success")->boolValue() &&
            setVarResp.find("body")->find("value")->stringValue() == "300",
        "DAP setVariable over TCP should update live local 'damage' to 300");

    // 8b. Request ManagedHeap Snapshot & Retaining Path over TCP (`heapSnapshot` + `retainingPath` + `:retaining`)
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 60},
        {"type", "request"},
        {"command", "heapSnapshot"},
        {"arguments", tooling::Json::Object{{"diffBaseline", false}}},
    });
    dapStream.flush();
    const auto snapResp = readNextDapMessage(dapStream);
    require(
        snapResp.find("success")->boolValue() &&
            snapResp.find("body")->find("liveObjects")->integerValue() >= 2 &&
            snapResp.find("body")->find("summaryText")->stringValue().find("Game.Combat::LootCache") != std::string::npos &&
            snapResp.find("body")->find("summaryText")->stringValue().find("retainedLoot") != std::string::npos,
        "DAP heapSnapshot must report live BossUnit & LootCache objects and .retainedLoot edge");

    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 61},
        {"type", "request"},
        {"command", "retainingPath"},
        {"arguments", tooling::Json::Object{
            {"expression", "this.retainedLoot"},
            {"frameId", 1},
        }},
    });
    dapStream.flush();
    const auto retainResp = readNextDapMessage(dapStream);
    const std::string retainChain = retainResp.find("body") && retainResp.find("body")->find("retainingPath")
        ? retainResp.find("body")->find("retainingPath")->stringValue()
        : "";
    require(
        retainResp.find("success")->boolValue() &&
            retainChain.find("Game.Combat::BossUnit") != std::string::npos &&
            retainChain.find(".retainedLoot") != std::string::npos &&
            retainChain.find("Game.Combat::LootCache") != std::string::npos,
        "DAP retainingPath must trace Root -> BossUnit --(.retainedLoot)--> LootCache, got: " + retainChain);
    std::cout << "[VS Code Client] Retaining Path verified: " << retainChain << "\n";

    // Also verify Debug Console REPL `:retaining this.retainedLoot` and `:heap diff`
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 62},
        {"type", "request"},
        {"command", "evaluate"},
        {"arguments", tooling::Json::Object{
            {"expression", ":retaining this.retainedLoot"},
            {"frameId", 1},
        }},
    });
    dapStream.flush();
    const auto replRetainResp = readNextDapMessage(dapStream);
    require(
        replRetainResp.find("success")->boolValue() &&
            replRetainResp.find("body")->find("result")->stringValue() == retainChain,
        "Debug Console REPL ':retaining this.retainedLoot' must return the exact retaining path");

    // 9. Send `disconnect` while paused -> must cleanly detach and resume the engine thread without terminating!
    tooling::writeProtocolMessage(dapStream, tooling::Json::Object{
        {"seq", 7},
        {"type", "request"},
        {"command", "disconnect"},
        {"arguments", tooling::Json::Object{{"terminateDebuggee", false}}},
    });
    dapStream.flush();
    (void)readNextDapMessage(dapStream);
    closeTestSocket(sock);

    engineThread.join();
    // Initial hp = 1000, damage mutated to 300 -> resulting hp must be 700!
    require(
        frameResult.succeeded && std::get<std::int64_t>(frameResult.value) == 700,
        "After DAP setVariable(damage=300) and disconnect, engine frame must finish with hp=700");

    // 10. Verify subsequent engine frames run without blocking after debugger detached
    const auto nextFrame = scene.invoke(1, "ApplyHit", {std::int64_t{90}}); // damage = 100 -> hp = 600
    require(
        nextFrame.succeeded && std::get<std::int64_t>(nextFrame.value) == 600,
        "Subsequent engine frame after DAP detach must execute normally without blocking (hp=600)");

    rt.stopRemoteDebugger();
    std::cout << "[PASS] End-to-end TCP Socket DAP Attach, Watch, SetVariable, and Detach verified!\n";
    return 0;
}
