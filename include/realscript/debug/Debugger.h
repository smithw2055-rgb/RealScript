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
    Exception,
};

enum class ResumeMode {
    Continue,
    StepIn,
    StepOver,
    StepOut,
    Terminate,
};

struct BreakpointSpec {
    text::LinePosition position{};
    std::string condition;
};

struct Breakpoint {
    std::uint32_t id = 0;
    std::string sourcePath;
    std::size_t line = 0;
    std::size_t column = 0;
    bool verified = false;
    std::string condition;
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
    runtime::ErrorCode errorCode = runtime::ErrorCode::None;
    std::string exceptionId;
    std::string exceptionDescription;
    bool exceptionUncaught = true;
};

struct DebugValueWindow {
    std::vector<runtime::Value>* values = nullptr;
    std::size_t offset = 0;
    std::size_t count = 0;

    [[nodiscard]] std::size_t size() const noexcept { return count; }
    [[nodiscard]] runtime::Value& operator[](std::size_t index) {
        return (*values)[offset + index];
    }
    [[nodiscard]] const runtime::Value& operator[](std::size_t index) const {
        return (*values)[offset + index];
    }
};

struct DebugFrameView {
    const bytecode::Module* module = nullptr;
    const bytecode::Function* function = nullptr;
    const SequencePoint* point = nullptr;
    DebugValueWindow arguments;
    DebugValueWindow locals;
    DebugValueWindow registers;
    bytecode::BlockId activeBlockId = 0;
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
    [[nodiscard]] std::vector<Breakpoint> setConditionalBreakpoints(
        std::string sourcePath,
        const std::vector<BreakpointSpec>& specs);
    [[nodiscard]] std::vector<Breakpoint> breakpoints() const;
    void clearBreakpoints();

    void setStopHandler(StopHandler handler);
    void stopOnEntry(bool enabled) noexcept;
    void setExceptionBreakpoints(
        bool breakOnThrown,
        bool breakOnUncaught,
        bool breakOnRuntimeError = false) noexcept;
    [[nodiscard]] bool breakOnThrown() const noexcept;
    [[nodiscard]] bool breakOnUncaught() const noexcept;
    [[nodiscard]] bool breakOnRuntimeError() const noexcept;
    void requestPause() noexcept;
    void resetTermination() noexcept;
    [[nodiscard]] std::string resolveSourcePath(const std::string& enginePath) const;

    [[nodiscard]] bool onSequencePoint(
        const std::vector<DebugFrameView>& frames,
        runtime::ManagedHeap* heap);
    [[nodiscard]] bool onException(
        const std::vector<DebugFrameView>& frames,
        runtime::ManagedHeap* heap,
        runtime::ErrorCode code,
        std::string exceptionId,
        std::string description,
        bool isUncaught,
        bool isRuntimeError);

    [[nodiscard]] std::optional<runtime::Value> evaluate(
        const std::string& expression,
        std::uint32_t frameId = 1,
        runtime::ManagedHeap* heap = nullptr,
        std::string* errorMessage = nullptr) const;
    bool setVariable(
        const std::string& name,
        const std::string& valueExpression,
        std::uint32_t frameId = 1,
        runtime::ManagedHeap* heap = nullptr,
        runtime::Value* outNewValue = nullptr,
        std::string* errorMessage = nullptr);

    [[nodiscard]] DebugStop lastStop() const;
    [[nodiscard]] bool terminated() const;

    void beginExecution();
    void endExecution() noexcept;
    [[nodiscard]] bool hasPendingInterruption() const noexcept;
    void runAtSafepoint(const std::function<void()>& callback) const;

private:
    struct BoundLocation {
        std::string sourcePath;
        std::size_t line = 0;
        std::size_t column = 0;
    };

    [[nodiscard]] std::optional<runtime::Value> evaluateInFrameUnlocked(
        const std::string& expression,
        const DebugStackFrame& frame,
        runtime::ManagedHeap* heap,
        std::string* errorMessage = nullptr) const;
    [[nodiscard]] std::uint32_t matchingBreakpoint(
        const std::vector<DebugFrameView>& frames,
        runtime::ManagedHeap* heap) const;
    [[nodiscard]] bool shouldStop(
        const std::vector<DebugFrameView>& frames,
        runtime::ManagedHeap* heap,
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
    std::unordered_map<std::string, std::string> clientPathAliases_;
    std::unordered_map<semantic::SymbolId, semantic::TypeSymbol> types_;
    runtime::ManagedHeap* lastHeap_ = nullptr;
    StopHandler stopHandler_;
    ResumeMode mode_ = ResumeMode::Continue;
    std::size_t stepDepth_ = 0;
    SourceRange lastLocation_;
    SourceRange lastBreakpointLocation_;
    bool stopOnEntry_ = false;
    bool entryPending_ = false;
    bool breakOnThrown_ = false;
    bool breakOnUncaught_ = false;
    bool breakOnRuntimeError_ = false;
    std::atomic<bool> pauseRequested_{false};
    bool terminated_ = false;
    std::uint32_t nextBreakpointId_ = 1;
    DebugStop lastStop_;
    const std::vector<DebugFrameView>* activeFrameViews_ = nullptr;
    struct ActiveThreadState {
        std::size_t depth = 0;
        bool stopped = false;
    };

    mutable std::mutex safepointMutex_;
    mutable std::condition_variable safepointCv_;
    std::unordered_map<std::thread::id, ActiveThreadState> activeThreads_;
    std::size_t activeExecutions_ = 0;
    std::size_t stoppedThreads_ = 0;
    mutable bool safepointActive_ = false;
    mutable std::thread::id safepointOwner_{};
    mutable std::atomic<bool> safepointRequested_{false};
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
    bool attach(bool stopOnEntry = false);
    void detach();
    void rebindProgram(std::shared_ptr<const runtime::ProgramImage> program);
    [[nodiscard]] bool isAttached() const noexcept;
    [[nodiscard]] bool waitForStop(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
    [[nodiscard]] std::optional<DebugStop> currentStop() const;
    [[nodiscard]] std::optional<runtime::Value> evaluate(
        const std::string& expression,
        std::uint32_t frameId = 1,
        std::string* errorMessage = nullptr) const;
    bool setVariable(
        const std::string& name,
        const std::string& valueExpression,
        std::uint32_t frameId = 1,
        runtime::Value* outNewValue = nullptr,
        std::string* errorMessage = nullptr);
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
    void runAtSafepoint(const std::function<void()>& callback) const;

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
    bool attached_ = false;
    bool terminateRequested_ = false;
};

[[nodiscard]] const char* stopReasonName(StopReason reason) noexcept;
[[nodiscard]] const char* resumeModeName(ResumeMode mode) noexcept;

} // namespace realscript::debug
