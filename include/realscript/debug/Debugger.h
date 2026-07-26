#pragma once

#include "realscript/debug/DebugInfo.h"
#include "realscript/runtime/Runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

namespace realscript::debug {

enum class StopReason {
    Entry,
    Breakpoint,
    Step,
    Pause,
};

enum class ResumeMode {
    Continue,
    StepIn,
    StepOver,
    StepOut,
    Terminate,
};

struct Breakpoint {
    std::uint32_t id = 0;
    std::string sourcePath;
    std::size_t line = 0;
    std::size_t column = 0;
    bool verified = false;
};

struct DebugVariable {
    std::string name;
    runtime::Value value;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
};

struct DebugStackFrame {
    std::uint32_t id = 0;
    std::string function;
    std::string sourcePath;
    SourceRange location;
    std::vector<DebugVariable> arguments;
    std::vector<DebugVariable> locals;
};

struct DebugStop {
    StopReason reason = StopReason::Breakpoint;
    std::uint32_t breakpointId = 0;
    std::vector<DebugStackFrame> frames;
};

struct DebugFrameView {
    const bytecode::Module* module = nullptr;
    const bytecode::Function* function = nullptr;
    const SequencePoint* point = nullptr;
    const std::vector<runtime::Value>* arguments = nullptr;
    const std::vector<runtime::Value>* locals = nullptr;
    const std::vector<runtime::Value>* registers = nullptr;
};

using StopHandler = std::function<ResumeMode(const DebugStop&)>;

class DebugController {
public:
    DebugController() = default;

    void bindProgram(const runtime::ProgramImage& program);
    void bindModules(const std::vector<bytecode::Module>& modules);
    [[nodiscard]] std::vector<Breakpoint> setBreakpoints(
        std::string sourcePath,
        const std::vector<text::LinePosition>& positions);
    [[nodiscard]] std::vector<Breakpoint> breakpoints() const;
    void clearBreakpoints();

    void setStopHandler(StopHandler handler);
    void stopOnEntry(bool enabled) noexcept;
    void requestPause() noexcept;

    [[nodiscard]] bool onSequencePoint(
        const std::vector<DebugFrameView>& frames,
        runtime::ManagedHeap* heap);

    [[nodiscard]] DebugStop lastStop() const;
    [[nodiscard]] bool terminated() const;

private:
    struct BoundLocation {
        std::string sourcePath;
        std::size_t line = 0;
        std::size_t column = 0;
    };

    [[nodiscard]] std::uint32_t matchingBreakpoint(
        const SequencePoint& point,
        const bytecode::Module& module) const;
    [[nodiscard]] bool shouldStop(
        const std::vector<DebugFrameView>& frames,
        StopReason& reason,
        std::uint32_t& breakpointId);
    [[nodiscard]] DebugStop capture(
        StopReason reason,
        std::uint32_t breakpointId,
        const std::vector<DebugFrameView>& frames,
        runtime::ManagedHeap* heap) const;

    mutable std::mutex mutex_;
    std::vector<Breakpoint> breakpoints_;
    std::unordered_map<std::string, std::vector<BoundLocation>> executableLocations_;
    StopHandler stopHandler_;
    ResumeMode mode_ = ResumeMode::Continue;
    std::size_t stepDepth_ = 0;
    SourceRange lastLocation_;
    bool stopOnEntry_ = false;
    bool entryPending_ = false;
    std::atomic<bool> pauseRequested_{false};
    bool terminated_ = false;
    std::uint32_t nextBreakpointId_ = 1;
    DebugStop lastStop_;
};


class DebugSession {
public:
    explicit DebugSession(
        std::shared_ptr<const runtime::ProgramImage> program,
        std::shared_ptr<runtime::ManagedHeap> heap = {});
    ~DebugSession();

    DebugSession(const DebugSession&) = delete;
    DebugSession& operator=(const DebugSession&) = delete;

    [[nodiscard]] std::shared_ptr<DebugController> controller() const noexcept;
    bool launch(
        std::string qualifiedFunction,
        std::vector<runtime::Value> arguments = {},
        bool stopOnEntry = false,
        runtime::Limits limits = {});
    [[nodiscard]] bool waitForStop(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    [[nodiscard]] std::optional<DebugStop> currentStop() const;
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] std::optional<runtime::ExecutionResult> result() const;

    void resume(ResumeMode mode);
    void continueExecution();
    void stepIn();
    void stepOver();
    void stepOut();
    void pause();
    void terminate();
    void join();

private:
    ResumeMode handleStop(const DebugStop& stop);

    std::shared_ptr<const runtime::ProgramImage> program_;
    std::shared_ptr<runtime::ManagedHeap> heap_;
    std::shared_ptr<DebugController> controller_;
    mutable std::mutex sessionMutex_;
    std::condition_variable sessionCv_;
    std::thread worker_;
    std::optional<DebugStop> currentStop_;
    std::optional<runtime::ExecutionResult> result_;
    ResumeMode pendingMode_ = ResumeMode::Continue;
    bool resumePending_ = false;
    bool paused_ = false;
    bool finished_ = false;
    bool launched_ = false;
    bool terminateRequested_ = false;
};

[[nodiscard]] const char* stopReasonName(StopReason reason) noexcept;
[[nodiscard]] const char* resumeModeName(ResumeMode mode) noexcept;

} // namespace realscript::debug
