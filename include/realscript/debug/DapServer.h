#pragma once

#include "realscript/debug/Debugger.h"
#include "realscript/tooling/Json.h"

#include <atomic>
#include <cstdint>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <unordered_map>

namespace realscript::debug {

class DapServer {
public:
    static constexpr std::int64_t kFirstChildVariablesReference = 1000000;

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

    [[nodiscard]] tooling::Json buildHeapSnapshotReport(bool diffBaseline = false);
    [[nodiscard]] std::string formatRetainingPathReport(runtime::ObjectRef target) const;

private:
    [[nodiscard]] tooling::Json response(
        const tooling::Json& request,
        bool success,
        tooling::Json body = nullptr,
        std::string message = {});
    void emitStopped(std::ostream& output);
    void emitTerminated(std::ostream& output);
    void rebuildVariableReferences();
    [[nodiscard]] std::int64_t registerExpandableValue(
        const runtime::Value& value,
        std::uint32_t ownerFrameId = 1u);
    [[nodiscard]] std::string describeValueType(const runtime::Value& value, semantic::PrimitiveType fallback = semantic::PrimitiveType::Error) const;
    void collectActiveStackValues(std::vector<runtime::Value>& outValues) const;
    [[nodiscard]] std::string describeHeapObject(
        const runtime::HeapObjectInfo& obj) const;
    [[nodiscard]] std::string describeEdgeLabel(
        const runtime::HeapObjectInfo& owner,
        const std::string& rawLabel) const;
    [[nodiscard]] std::string describeRootInfo(
        const runtime::HeapRootInfo& root) const;

    std::shared_ptr<const runtime::ProgramImage> program_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<DebugSession> session_;
    std::atomic<std::uint32_t> sequence_{1};
    std::unordered_map<std::int64_t, std::vector<DebugVariable>> variables_;
    std::unordered_map<std::int64_t, runtime::ObjectRef> childOwnerObjects_;
    std::unordered_map<std::int64_t, std::uint32_t> variableFrameIds_;
    std::optional<runtime::HeapSnapshot> baselineSnapshot_;
    std::int64_t nextChildRef_ = kFirstChildVariablesReference;
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
