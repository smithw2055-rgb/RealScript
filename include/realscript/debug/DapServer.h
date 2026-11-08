#pragma once

#include "realscript/debug/Debugger.h"
#include "realscript/tooling/Json.h"

#include <atomic>
#include <cstdint>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <unordered_map>

namespace realscript::debug {

class DapServer {
public:
    explicit DapServer(
        std::shared_ptr<const runtime::ProgramImage> program,
        std::shared_ptr<runtime::ManagedHeap> heap = {});
    explicit DapServer(
        std::shared_ptr<DebugSession> session,
        std::shared_ptr<const runtime::ProgramImage> program,
        std::shared_ptr<runtime::ManagedHeap> heap);

    int run(std::istream& input, std::ostream& output);
    int serveTcp(
        std::uint16_t port = 4711,
        const std::string& bindAddress = "127.0.0.1",
        std::uint16_t* outBoundPort = nullptr);

    [[nodiscard]] tooling::Json handle(
        const tooling::Json& request,
        std::ostream* events = nullptr);

    [[nodiscard]] std::shared_ptr<DebugSession> session() const noexcept { return session_; }
    [[nodiscard]] std::shared_ptr<DebugController> controller() const noexcept {
        return session_ ? session_->controller() : nullptr;
    }
    void rebindProgram(std::shared_ptr<const runtime::ProgramImage> program);

private:
    [[nodiscard]] tooling::Json response(
        const tooling::Json& request,
        bool success,
        tooling::Json body = nullptr,
        std::string message = {});
    void emitStopped(std::ostream& output);
    void emitTerminated(std::ostream& output);
    void rebuildVariableReferences();
    [[nodiscard]] std::int64_t registerExpandableValue(const runtime::Value& value);

    std::shared_ptr<const runtime::ProgramImage> program_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<DebugSession> session_;
    std::atomic<std::uint32_t> sequence_{1};
    std::unordered_map<std::int64_t, std::vector<DebugVariable>> variables_;
    std::unordered_map<std::int64_t, runtime::ObjectRef> childOwnerObjects_;
    std::int64_t nextChildRef_ = 1000;
};

class EmbeddedDapServer {
public:
    explicit EmbeddedDapServer(
        std::shared_ptr<const runtime::ProgramImage> program,
        std::shared_ptr<runtime::ManagedHeap> heap = {});
    ~EmbeddedDapServer();

    EmbeddedDapServer(const EmbeddedDapServer&) = delete;
    EmbeddedDapServer& operator=(const EmbeddedDapServer&) = delete;

    bool start(
        std::uint16_t port = 4711,
        const std::string& bindAddress = "127.0.0.1");
    void stop() noexcept;

    [[nodiscard]] bool isRunning() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint16_t boundPort() const noexcept {
        return boundPort_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::shared_ptr<DebugSession> session() const noexcept {
        return session_;
    }
    [[nodiscard]] std::shared_ptr<DebugController> controller() const noexcept {
        return session_ ? session_->controller() : nullptr;
    }
    void rebindProgram(std::shared_ptr<const runtime::ProgramImage> program);

private:
    std::shared_ptr<const runtime::ProgramImage> program_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<DebugSession> session_;
    mutable std::mutex mutex_;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<std::uint16_t> boundPort_{0};
    std::intptr_t listenSocket_{-1};
    std::intptr_t activeClientSocket_{-1};
};

} // namespace realscript::debug
