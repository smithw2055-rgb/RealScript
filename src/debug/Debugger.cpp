#include "realscript/debug/Debugger.h"

#include <algorithm>
#include <utility>

namespace realscript::debug {
namespace {

bool sameLocation(const SourceRange& left, const SourceRange& right) noexcept {
    return left.fileId == right.fileId &&
        left.start.line == right.start.line &&
        left.start.column == right.start.column;
}

const SourceFileInfo* sourceFile(
    const bytecode::Module& module,
    SourceFileId id) noexcept {
    for (const auto& source : module.sourceFiles) {
        if (source.id == id) return &source;
    }
    return nullptr;
}

} // namespace

void DebugController::bindProgram(const runtime::ProgramImage& program) {
    bindModules(program.modules());
}

void DebugController::bindModules(const std::vector<bytecode::Module>& modules) {
    std::lock_guard<std::mutex> lock(mutex_);
    executableLocations_.clear();
    for (const auto& module : modules) {
        for (const auto& function : module.functions) {
            for (const auto& point : function.debugInfo.sequencePoints) {
                const auto* source = sourceFile(module, point.range.fileId);
                if (!source) continue;
                executableLocations_[source->path].push_back({
                    source->path,
                    point.range.start.line,
                    point.range.start.column,
                });
            }
        }
    }
    for (auto& [path, locations] : executableLocations_) {
        (void)path;
        std::sort(locations.begin(), locations.end(),
            [](const BoundLocation& left, const BoundLocation& right) {
                if (left.line != right.line) return left.line < right.line;
                return left.column < right.column;
            });
        locations.erase(std::unique(locations.begin(), locations.end(),
            [](const BoundLocation& left, const BoundLocation& right) {
                return left.line == right.line && left.column == right.column;
            }), locations.end());
    }
    for (auto& breakpoint : breakpoints_) {
        breakpoint.verified = false;
        const auto found = executableLocations_.find(breakpoint.sourcePath);
        if (found == executableLocations_.end()) continue;
        const auto location = std::lower_bound(
            found->second.begin(), found->second.end(), breakpoint.line,
            [](const BoundLocation& value, std::size_t line) {
                return value.line < line;
            });
        if (location != found->second.end()) {
            breakpoint.line = location->line;
            breakpoint.column = location->column;
            breakpoint.verified = true;
        }
    }
    entryPending_ = stopOnEntry_;
}

std::vector<Breakpoint> DebugController::setBreakpoints(
    std::string sourcePath,
    const std::vector<text::LinePosition>& positions) {
    std::lock_guard<std::mutex> lock(mutex_);
    breakpoints_.erase(
        std::remove_if(breakpoints_.begin(), breakpoints_.end(),
            [&](const Breakpoint& breakpoint) {
                return breakpoint.sourcePath == sourcePath;
            }),
        breakpoints_.end());

    const auto executable = executableLocations_.find(sourcePath);
    for (const auto position : positions) {
        Breakpoint breakpoint;
        breakpoint.id = nextBreakpointId_++;
        breakpoint.sourcePath = sourcePath;
        breakpoint.line = position.line;
        breakpoint.column = position.column;
        if (executable != executableLocations_.end()) {
            const auto exact = std::lower_bound(
                executable->second.begin(), executable->second.end(), position.line,
                [](const BoundLocation& location, std::size_t line) {
                    return location.line < line;
                });
            if (exact != executable->second.end()) {
                breakpoint.line = exact->line;
                breakpoint.column = exact->column;
                breakpoint.verified = true;
            }
        }
        breakpoints_.push_back(std::move(breakpoint));
    }
    return breakpoints_;
}

std::vector<Breakpoint> DebugController::breakpoints() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return breakpoints_;
}

void DebugController::clearBreakpoints() {
    std::lock_guard<std::mutex> lock(mutex_);
    breakpoints_.clear();
}

void DebugController::setStopHandler(StopHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    stopHandler_ = std::move(handler);
}

void DebugController::stopOnEntry(bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stopOnEntry_ = enabled;
    entryPending_ = enabled;
}

void DebugController::requestPause() noexcept {
    pauseRequested_.store(true);
}

std::uint32_t DebugController::matchingBreakpoint(
    const SequencePoint& point,
    const bytecode::Module& module) const {
    const auto* source = sourceFile(module, point.range.fileId);
    if (!source) return 0;
    for (const auto& breakpoint : breakpoints_) {
        if (breakpoint.verified &&
            breakpoint.sourcePath == source->path &&
            breakpoint.line == point.range.start.line) {
            return breakpoint.id;
        }
    }
    return 0;
}

bool DebugController::shouldStop(
    const std::vector<DebugFrameView>& frames,
    StopReason& reason,
    std::uint32_t& breakpointId) {
    if (frames.empty() || !frames.back().point || !frames.back().module) {
        return false;
    }
    const auto& current = frames.back();
    const auto depth = frames.size();
    if (entryPending_) {
        entryPending_ = false;
        reason = StopReason::Entry;
        return true;
    }
    if (pauseRequested_.exchange(false)) {
        reason = StopReason::Pause;
        return true;
    }
    breakpointId = matchingBreakpoint(*current.point, *current.module);
    if (breakpointId != 0) {
        reason = StopReason::Breakpoint;
        return true;
    }
    const auto changed = !sameLocation(current.point->range, lastLocation_);
    switch (mode_) {
    case ResumeMode::StepIn:
        if (changed) {
            reason = StopReason::Step;
            return true;
        }
        break;
    case ResumeMode::StepOver:
        if (changed && depth <= stepDepth_) {
            reason = StopReason::Step;
            return true;
        }
        break;
    case ResumeMode::StepOut:
        if (depth < stepDepth_) {
            reason = StopReason::Step;
            return true;
        }
        break;
    case ResumeMode::Terminate:
        terminated_ = true;
        return false;
    case ResumeMode::Continue:
        break;
    }
    return false;
}

DebugStop DebugController::capture(
    StopReason reason,
    std::uint32_t breakpointId,
    const std::vector<DebugFrameView>& frames,
    runtime::ManagedHeap* heap) const {
    (void)heap;
    DebugStop stop;
    stop.reason = reason;
    stop.breakpointId = breakpointId;
    std::uint32_t frameId = 1;
    for (auto view = frames.rbegin(); view != frames.rend(); ++view) {
        if (!view->module || !view->function) continue;
        DebugStackFrame frame;
        frame.id = frameId++;
        frame.function = view->module->name + "::" + view->function->name;
        if (view->point) {
            frame.location = view->point->range;
            if (const auto* source = sourceFile(*view->module, view->point->range.fileId)) {
                frame.sourcePath = source->path;
            }
        }
        for (const auto& local : view->function->debugInfo.locals) {
            const auto* values = view->locals;
            if (!values || local.slot >= values->size()) continue;
            if (!local.parameter && view->point) {
                const auto offset = view->point->range.span.start;
                if (offset < local.scope.span.start ||
                    offset > local.scope.span.end()) continue;
            }
            DebugVariable variable;
            variable.name = local.name;
            variable.value = (*values)[local.slot];
            variable.type = local.type;
            variable.typeId = local.typeId;
            if (local.parameter) {
                frame.arguments.push_back(std::move(variable));
            } else {
                frame.locals.push_back(std::move(variable));
            }
        }
        stop.frames.push_back(std::move(frame));
    }
    return stop;
}

bool DebugController::onSequencePoint(
    const std::vector<DebugFrameView>& frames,
    runtime::ManagedHeap* heap) {
    StopHandler handler;
    StopReason reason = StopReason::Breakpoint;
    std::uint32_t breakpointId = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_) return false;
        if (!shouldStop(frames, reason, breakpointId)) return true;
        lastStop_ = capture(reason, breakpointId, frames, heap);
        handler = stopHandler_;
    }

    const auto next = handler ? handler(lastStop_) : ResumeMode::Continue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = next;
        stepDepth_ = frames.size();
        if (!frames.empty() && frames.back().point) {
            lastLocation_ = frames.back().point->range;
        }
        if (next == ResumeMode::Terminate) {
            terminated_ = true;
            return false;
        }
    }
    return true;
}

DebugStop DebugController::lastStop() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastStop_;
}
bool DebugController::terminated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminated_;
}

const char* stopReasonName(StopReason reason) noexcept {
    switch (reason) {
    case StopReason::Entry: return "entry";
    case StopReason::Breakpoint: return "breakpoint";
    case StopReason::Step: return "step";
    case StopReason::Pause: return "pause";
    }
    return "unknown";
}

const char* resumeModeName(ResumeMode mode) noexcept {
    switch (mode) {
    case ResumeMode::Continue: return "continue";
    case ResumeMode::StepIn: return "step-in";
    case ResumeMode::StepOver: return "step-over";
    case ResumeMode::StepOut: return "step-out";
    case ResumeMode::Terminate: return "terminate";
    }
    return "unknown";
}

} // namespace realscript::debug

namespace realscript::debug {

DebugSession::DebugSession(
    std::shared_ptr<const runtime::ProgramImage> program,
    std::shared_ptr<runtime::ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<runtime::ManagedHeap>()),
      controller_(std::make_shared<DebugController>()) {
    if (program_) controller_->bindProgram(*program_);
    controller_->setStopHandler(
        [this](const DebugStop& stop) { return handleStop(stop); });
}

DebugSession::~DebugSession() {
    terminate();
    join();
}

std::shared_ptr<DebugController> DebugSession::controller() const noexcept {
    return controller_;
}

bool DebugSession::launch(
    std::string qualifiedFunction,
    std::vector<runtime::Value> arguments,
    bool stopOnEntry,
    runtime::Limits limits) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (launched_ || !program_) return false;
    launched_ = true;
    finished_ = false;
    paused_ = false;
    result_.reset();
    currentStop_.reset();
    terminateRequested_ = false;
    controller_->stopOnEntry(stopOnEntry);
    worker_ = std::thread([
        this,
        function = std::move(qualifiedFunction),
        values = std::move(arguments),
        limits]() mutable {
        runtime::Interpreter interpreter(program_, heap_);
        runtime::ExecutionOptions options;
        options.limits = limits;
        options.debugger = controller_;
        auto execution = interpreter.invoke(function, values, std::move(options));
        {
            std::lock_guard<std::mutex> sessionLock(sessionMutex_);
            result_ = std::move(execution);
            finished_ = true;
            paused_ = false;
        }
        sessionCv_.notify_all();
    });
    return true;
}

bool DebugSession::waitForStop(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(sessionMutex_);
    const auto ready = [&] { return paused_ || finished_; };
    if (timeout.count() <= 0) {
        sessionCv_.wait(lock, ready);
        return paused_;
    }
    return sessionCv_.wait_for(lock, timeout, ready) && paused_;
}

std::optional<DebugStop> DebugSession::currentStop() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return currentStop_;
}

bool DebugSession::paused() const noexcept {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return paused_;
}

bool DebugSession::finished() const noexcept {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return finished_;
}

std::optional<runtime::ExecutionResult> DebugSession::result() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return result_;
}

void DebugSession::resume(ResumeMode mode) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (!paused_) return;
        pendingMode_ = mode;
        resumePending_ = true;
        paused_ = false;
    }
    sessionCv_.notify_all();
}

void DebugSession::continueExecution() { resume(ResumeMode::Continue); }
void DebugSession::stepIn() { resume(ResumeMode::StepIn); }
void DebugSession::stepOver() { resume(ResumeMode::StepOver); }
void DebugSession::stepOut() { resume(ResumeMode::StepOut); }
void DebugSession::pause() { controller_->requestPause(); }

void DebugSession::terminate() {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (!launched_ || finished_) return;
        pendingMode_ = ResumeMode::Terminate;
        terminateRequested_ = true;
        resumePending_ = true;
        paused_ = false;
    }
    controller_->requestPause();
    sessionCv_.notify_all();
}

void DebugSession::join() {
    if (worker_.joinable()) worker_.join();
}

ResumeMode DebugSession::handleStop(const DebugStop& stop) {
    std::unique_lock<std::mutex> lock(sessionMutex_);
    if (terminateRequested_) return ResumeMode::Terminate;
    currentStop_ = stop;
    paused_ = true;
    resumePending_ = false;
    sessionCv_.notify_all();
    sessionCv_.wait(lock, [&] { return resumePending_ || terminateRequested_; });
    const auto mode = terminateRequested_ ? ResumeMode::Terminate : pendingMode_;
    resumePending_ = false;
    return mode;
}

} // namespace realscript::debug
