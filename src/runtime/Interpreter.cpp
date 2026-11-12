#include <cstring>
#include "realscript/runtime/Runtime.h"
#include "realscript/runtime/AsyncRuntime.h"

#include "ExecutionMirror.h"
#include "realscript/debug/Debugger.h"
#include "realscript/diagnostics/Diagnostic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::runtime {
namespace {

struct InlineCallCacheEntry {
    const bytecode::Instruction* callSite = nullptr;
    ObjectRef receiver;
    semantic::SymbolId receiverTypeId = 0;
    const ProgramImage::FunctionLocation* target = nullptr;
    const detail::ExecutionFunction* execution = nullptr;
};

struct PolymorphicPicWay {
    semantic::SymbolId receiverTypeId = 0;
    const ProgramImage::FunctionLocation* target = nullptr;
    const detail::ExecutionFunction* execution = nullptr;
};

struct PolymorphicPicSite {
    std::array<PolymorphicPicWay, 4> ways{};
    std::uint8_t nextReplace = 0;
};

struct ResolvedDelegateLeaf {
    ObjectRef leafRef{};
    ObjectRef receiverRef{};
    Value receiverVal{NullObject{}};
    const ProgramImage::FunctionLocation* target = nullptr;
    const detail::ExecutionFunction* execution = nullptr;
    bool hasBoundReceiver = false;
    bool valid = false;
};

struct DelegateCallSiteCache {
    std::array<ResolvedDelegateLeaf, 16> singleWays{};

    semantic::SymbolId lastSymId = 0;
    const ProgramImage::FunctionLocation* lastSymTarget = nullptr;
    const detail::ExecutionFunction* lastSymExec = nullptr;

    ObjectRef multicastRef{};
    std::vector<ResolvedDelegateLeaf> multicastLeaves;
    bool multicastValid = false;
    std::uint32_t multicastPinCount = 0;
};

DeterminismOperationId instructionOperationId(
    bytecode::Opcode opcode) noexcept {
    constexpr auto Count =
        static_cast<std::size_t>(bytecode::Opcode::AsyncResolve) + 1U;
    static const auto ids = [] {
        std::array<DeterminismOperationId, Count> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = determinismOperationId(bytecode::opcodeName(
                static_cast<bytecode::Opcode>(index)));
        }
        return result;
    }();
    return ids[static_cast<std::size_t>(opcode)];
}

[[maybe_unused]] DeterminismOperationId terminatorOperationId(
    bytecode::TerminatorKind terminator) noexcept {
    constexpr auto Count =
        static_cast<std::size_t>(bytecode::TerminatorKind::Throw) + 1U;
    static const auto ids = [] {
        std::array<DeterminismOperationId, Count> result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = determinismOperationId(bytecode::terminatorName(
                static_cast<bytecode::TerminatorKind>(index)));
        }
        return result;
    }();
    return ids[static_cast<std::size_t>(terminator)];
}

struct State {
    Limits limits;
    std::uint64_t executed = 0;
    RuntimeStatistics statistics;
    RuntimeError error;
    std::vector<const std::string*> stack;
    const ProgramImage* program = nullptr;
    const ExternalFunction* externalResolver = nullptr;
    const BindingRegistry* bindings = nullptr;
    const TraceSink* trace = nullptr;
    ProfileCollector* profile = nullptr;
    bool traceEventsEnabled = false;
    bool determinismEventsEnabled = false;
    bool determinismOnly = false;
    bool diagnosticEventsEnabled = false;
    bool fastAccounting = false;
    std::uint64_t determinismEventDigest = DeterminismEventSeed;
    std::uint64_t determinismEventCount = 0;
    std::unordered_map<semantic::SymbolId, FunctionProfile> profileFunctions;
    std::uint64_t profileEvents = 0;
    DeterminismSession determinism;
    semantic::SymbolId currentFunctionId = 0;
    ManagedHeap* heap = nullptr;
    debug::DebugController* debugger = nullptr;
    std::vector<debug::DebugFrameView> debugFrames;
    ShadowStack shadowStack;
    std::vector<Value> values;
    std::size_t valueTop = 0;
    std::array<InlineCallCacheEntry, 64>* inlineCallCache = nullptr;
    std::vector<PolymorphicPicSite>* polyCallCache = nullptr;
    std::vector<DelegateCallSiteCache>* delegateCallCache = nullptr;
    const detail::ExecutionMirror* mirror = nullptr;
    std::optional<Value> pendingException;
    bool exceptionNotifiedDebugger = false;
    AsyncRuntimeContext* asyncRuntime = nullptr;
    void* resumeEnvRaw = nullptr;
    const std::shared_ptr<const void>* resumeEnvOwner = nullptr;
    CoroutineFrameHandle resumingCoroutine{};
    CoroutineFrame* resumingFramePtr = nullptr;
    CoroutineFrameHandle* currentBlockVmCoroutinePtr = nullptr;
    bool coroutineSuspendedNow = false;
    Value suspendedReturnValue{};
};

struct InterpreterResumeEnv {
    std::shared_ptr<const ProgramImage> program;
    std::shared_ptr<ManagedHeap> heap;
    std::shared_ptr<const BindingRegistry> bindings;
    ExternalFunction externalResolver;
    ExecutionOptions options;
};

struct ThreadInvokeScratch {
    struct Buffers {
        std::vector<Value> values;
        std::vector<const std::string*> stack;
        std::array<InlineCallCacheEntry, 64> inlineCallCache{};
        std::vector<PolymorphicPicSite> polyCallCache;
        std::vector<DelegateCallSiteCache> delegateCallCache;
        std::uint64_t lastMirrorId = 0;
    };
    enum : std::size_t { MaxDepth = 8 };
    std::array<Buffers, MaxDepth> pool{};
    std::size_t depth = 0;
};
thread_local ThreadInvokeScratch tlsInvokeScratch;

[[nodiscard]] constexpr bool isValidInt32Payload(std::int64_t v) noexcept {
    return v >= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) &&
        v <= static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
}

[[nodiscard]] constexpr bool fastFixedAdd(std::int32_t a, std::int32_t b, std::int32_t& out) noexcept {
    const std::int64_t r = static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
    if (!isValidInt32Payload(r)) return false;
    out = static_cast<std::int32_t>(r);
    return true;
}

[[nodiscard]] constexpr bool fastFixedSub(std::int32_t a, std::int32_t b, std::int32_t& out) noexcept {
    const std::int64_t r = static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b);
    if (!isValidInt32Payload(r)) return false;
    out = static_cast<std::int32_t>(r);
    return true;
}

[[nodiscard]] constexpr bool fastFixedNeg(std::int32_t a, std::int32_t& out) noexcept {
    if (a == std::numeric_limits<std::int32_t>::min()) return false;
    out = -a;
    return true;
}

[[nodiscard]] constexpr bool fastFixedMul(std::int32_t a, std::int32_t b, std::int32_t& out) noexcept {
    const std::int64_t r = (static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b)) / 256;
    if (!isValidInt32Payload(r)) return false;
    out = static_cast<std::int32_t>(r);
    return true;
}

[[nodiscard]] constexpr bool fastFixedDiv(std::int32_t a, std::int32_t b, std::int32_t& out) noexcept {
    if (b == 0) return false;
    const std::int64_t r = (static_cast<std::int64_t>(a) * 256) / static_cast<std::int64_t>(b);
    if (!isValidInt32Payload(r)) return false;
    out = static_cast<std::int32_t>(r);
    return true;
}

[[nodiscard]] constexpr std::int32_t fastEvalFixedSin(
    std::int32_t raw,
    std::uint64_t wrapExtraCost,
    std::uint64_t& ioExtraInst,
    std::uint64_t& ioBranches) noexcept {
    int r = raw % 1608;
    ioBranches += 6;
    if (r > 804) {
        r -= 1608;
        ioExtraInst += wrapExtraCost;
        ++ioBranches;
    }
    if (r < -804) {
        r += 1608;
        ioExtraInst += wrapExtraCost;
        ++ioBranches;
    }
    int absR = r;
    if (r < 0) {
        absR = -r;
        ++ioExtraInst;
    }
    const std::int64_t y0 =
        (static_cast<std::int64_t>(r) * static_cast<std::int64_t>(804 - absR) * 262144LL) /
        (804LL * 804LL);
    std::int64_t absY0 = y0;
    if (y0 < 0) {
        absY0 = -y0;
        ++ioExtraInst;
    }
    const std::int64_t y = (y0 * 775LL + ((y0 * absY0) / 65536LL) * 225LL) / 1000LL;
    return static_cast<std::int32_t>(y / 256LL);
}

[[nodiscard]] constexpr std::int64_t fastEvalSqrt64Pos(
    std::int64_t n,
    std::uint64_t loopIterCost,
    std::uint64_t& outLoopInst,
    std::uint64_t& outBranches) noexcept {
    std::uint64_t loopInst = 0;
    std::uint64_t br = 5;
    std::int64_t lo = 1;
    std::int64_t hi = n < 3037000499LL ? n : 3037000499LL;
    std::int64_t ans = 1;
    while (lo <= hi) {
        const std::int64_t mid = lo + (hi - lo) / 2;
        loopInst += loopIterCost;
        br += 4;
        if (mid <= n / mid) {
            ans = mid;
            lo = mid + 1;
            loopInst += 2;
        } else {
            hi = mid - 1;
        }
    }
    outLoopInst = loopInst;
    outBranches = br;
    return ans;
}

[[nodiscard]] constexpr bool fastVec3Cross(
    const Vec3Value& a,
    const Vec3Value& b,
    Vec3Value& out) noexcept {
    std::int32_t p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0;
    return fastFixedMul(a.y, b.z, p1) &&
           fastFixedMul(a.z, b.y, p2) &&
           fastFixedSub(p1, p2, out.x) &&
           fastFixedMul(a.z, b.x, p3) &&
           fastFixedMul(a.x, b.z, p4) &&
           fastFixedSub(p3, p4, out.y) &&
           fastFixedMul(a.x, b.y, p5) &&
           fastFixedMul(a.y, b.x, p6) &&
           fastFixedSub(p5, p6, out.z);
}

[[nodiscard]] constexpr bool fastQuatRotateVec3(
    const QuatValue& q,
    const Vec3Value& v,
    Vec3Value& out) noexcept {
    const Vec3Value u{q.x, q.y, q.z};
    Vec3Value uv{};
    Vec3Value uuv{};
    if (!fastVec3Cross(u, v, uv) || !fastVec3Cross(u, uv, uuv)) return false;
    std::int32_t uvwX = 0, uvwY = 0, uvwZ = 0;
    std::int32_t sumX = 0, sumY = 0, sumZ = 0;
    if (!fastFixedMul(uv.x, q.w, uvwX) ||
        !fastFixedMul(uv.y, q.w, uvwY) ||
        !fastFixedMul(uv.z, q.w, uvwZ) ||
        !fastFixedAdd(uvwX, uuv.x, sumX) ||
        !fastFixedAdd(uvwY, uuv.y, sumY) ||
        !fastFixedAdd(uvwZ, uuv.z, sumZ)) {
        return false;
    }
    const std::int64_t dblX = static_cast<std::int64_t>(sumX) * 2;
    const std::int64_t dblY = static_cast<std::int64_t>(sumY) * 2;
    const std::int64_t dblZ = static_cast<std::int64_t>(sumZ) * 2;
    if (!isValidInt32Payload(dblX) || !isValidInt32Payload(dblY) || !isValidInt32Payload(dblZ)) {
        return false;
    }
    return fastFixedAdd(v.x, static_cast<std::int32_t>(dblX), out.x) &&
           fastFixedAdd(v.y, static_cast<std::int32_t>(dblY), out.y) &&
           fastFixedAdd(v.z, static_cast<std::int32_t>(dblZ), out.z);
}

[[nodiscard]] constexpr bool fastQuatDot(
    const QuatValue& a,
    const QuatValue& b,
    std::int32_t& out) noexcept {
    std::int32_t px = 0, py = 0, pz = 0, pw = 0, s0 = 0, s1 = 0;
    return fastFixedMul(a.x, b.x, px) &&
           fastFixedMul(a.y, b.y, py) &&
           fastFixedAdd(px, py, s0) &&
           fastFixedMul(a.z, b.z, pz) &&
           fastFixedAdd(s0, pz, s1) &&
           fastFixedMul(a.w, b.w, pw) &&
           fastFixedAdd(s1, pw, out);
}

[[nodiscard]] constexpr bool fastQuatMul(
    const QuatValue& a,
    const QuatValue& b,
    QuatValue& out) noexcept {
    std::int32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, s0 = 0, s1 = 0;
    if (!(fastFixedMul(a.w, b.x, t0) && fastFixedMul(a.x, b.w, t1) && fastFixedAdd(t0, t1, s0) &&
          fastFixedMul(a.y, b.z, t2) && fastFixedAdd(s0, t2, s1) &&
          fastFixedMul(a.z, b.y, t3) && fastFixedSub(s1, t3, out.x))) {
        return false;
    }
    if (!(fastFixedMul(a.w, b.y, t0) && fastFixedMul(a.x, b.z, t1) && fastFixedSub(t0, t1, s0) &&
          fastFixedMul(a.y, b.w, t2) && fastFixedAdd(s0, t2, s1) &&
          fastFixedMul(a.z, b.x, t3) && fastFixedAdd(s1, t3, out.y))) {
        return false;
    }
    if (!(fastFixedMul(a.w, b.z, t0) && fastFixedMul(a.x, b.y, t1) && fastFixedAdd(t0, t1, s0) &&
          fastFixedMul(a.y, b.x, t2) && fastFixedSub(s0, t2, s1) &&
          fastFixedMul(a.z, b.w, t3) && fastFixedAdd(s1, t3, out.z))) {
        return false;
    }
    return fastFixedMul(a.w, b.w, t0) && fastFixedMul(a.x, b.x, t1) && fastFixedSub(t0, t1, s0) &&
           fastFixedMul(a.y, b.y, t2) && fastFixedSub(s0, t2, s1) &&
           fastFixedMul(a.z, b.z, t3) && fastFixedSub(s1, t3, out.w);
}

[[nodiscard]] constexpr bool fastQuatNormalize(
    const QuatValue& q,
    std::uint64_t loopIterCost,
    QuatValue& out,
    std::uint64_t& outLoopInst,
    std::uint64_t& outBranches) noexcept {
    const std::int64_t xx = static_cast<std::int64_t>(q.x) * q.x;
    const std::int64_t yy = static_cast<std::int64_t>(q.y) * q.y;
    const std::int64_t zz = static_cast<std::int64_t>(q.z) * q.z;
    const std::int64_t ww = static_cast<std::int64_t>(q.w) * q.w;
    if (xx > std::numeric_limits<std::int64_t>::max() - yy ||
        xx + yy > std::numeric_limits<std::int64_t>::max() - zz ||
        xx + yy + zz > std::numeric_limits<std::int64_t>::max() - ww) {
        return false;
    }
    const std::int64_t r2 = xx + yy + zz + ww;
    if (r2 <= 0) return false;
    std::uint64_t loopInst = 0;
    std::uint64_t sqBr = 0;
    const std::int64_t len64 = fastEvalSqrt64Pos(r2, loopIterCost, loopInst, sqBr);
    if (!isValidInt32Payload(len64) || len64 <= 0) return false;
    const auto lenRaw = static_cast<std::int32_t>(len64);
    if (!(fastFixedDiv(q.x, lenRaw, out.x) &&
          fastFixedDiv(q.y, lenRaw, out.y) &&
          fastFixedDiv(q.z, lenRaw, out.z) &&
          fastFixedDiv(q.w, lenRaw, out.w))) {
        return false;
    }
    outLoopInst = loopInst;
    outBranches = 1u + sqBr;
    return true;
}

struct ValueWindow {
    std::vector<Value>* values = nullptr;
    std::size_t offset = 0;
    std::size_t count = 0;

    [[nodiscard]] std::size_t size() const noexcept { return count; }
    [[nodiscard]] Value& operator[](std::size_t index) {
        return (*values)[offset + index];
    }
    [[nodiscard]] const Value& operator[](std::size_t index) const {
        return (*values)[offset + index];
    }
    [[nodiscard]] debug::DebugValueWindow debugView() const noexcept {
        return {values, offset, count};
    }
};

struct ArgumentSource {
    const std::vector<Value>* values = nullptr;
    const Value* directValues = nullptr;
    std::size_t offset = 0;
    std::array<bytecode::Register, 3> inlineRegs{0, 0, 0};
    const bytecode::Register* indices = nullptr;
    std::size_t count = 0;

    ArgumentSource() = default;
    ArgumentSource(const std::vector<Value>& contiguous)
        : values(&contiguous), count(contiguous.size()) {}
    ArgumentSource(const Value* ptr, std::size_t cnt) noexcept
        : directValues(ptr), count(cnt) {}
    ArgumentSource(
        const ValueWindow& registers,
        const bytecode::SmallRegisterVector& operands) noexcept
        : values(registers.values),
          offset(registers.offset),
          indices(operands.data()),
          count(operands.size()) {}
    ArgumentSource(
        const ValueWindow& registers,
        bytecode::Register reg0) noexcept
        : values(registers.values),
          offset(registers.offset),
          inlineRegs{reg0, 0, 0},
          indices(inlineRegs.data()),
          count(1) {}
    ArgumentSource(
        const ValueWindow& registers,
        bytecode::Register reg0,
        bytecode::Register reg1) noexcept
        : values(registers.values),
          offset(registers.offset),
          inlineRegs{reg0, reg1, 0},
          indices(inlineRegs.data()),
          count(2) {}
    ArgumentSource(
        const ValueWindow& registers,
        bytecode::Register reg0,
        bytecode::Register reg1,
        bytecode::Register reg2) noexcept
        : values(registers.values),
          offset(registers.offset),
          inlineRegs{reg0, reg1, reg2},
          indices(inlineRegs.data()),
          count(3) {}
    ArgumentSource(
        const ValueWindow& registers) noexcept
        : values(registers.values),
          offset(registers.offset),
          indices(nullptr),
          count(0) {}

    [[nodiscard]] std::size_t size() const noexcept { return count; }
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
    [[nodiscard]] const Value& operator[](std::size_t index) const noexcept {
        if (directValues) return directValues[index];
        return (*values)[offset + (indices ? indices[index] : index)];
    }
    [[nodiscard]] const Value& front() const noexcept { return (*this)[0]; }
};

class ArenaFrameScope {
public:
    explicit ArenaFrameScope(State& state)
        : state_(state), base_(state.valueTop) {}
    ~ArenaFrameScope() {
        state_.valueTop = base_;
    }
    ArenaFrameScope(const ArenaFrameScope&) = delete;
    ArenaFrameScope& operator=(const ArenaFrameScope&) = delete;
    [[nodiscard]] std::size_t base() const noexcept { return base_; }
    void allocate(std::size_t count) {
        const std::size_t previousSize = state_.values.size();
        top_ = base_ + count;
        if (previousSize < top_) {
            if (base_ < previousSize) {
                std::memset(static_cast<void*>(state_.values.data() + base_), 0,
                    (previousSize - base_) * sizeof(Value));
            }
            state_.values.resize(top_);
        } else if (base_ < top_) {
            std::memset(static_cast<void*>(state_.values.data() + base_), 0,
                (top_ - base_) * sizeof(Value));
        }
        state_.valueTop = top_;
    }

private:
    State& state_;
    std::size_t base_ = 0;
    std::size_t top_ = 0;
};

std::vector<std::string> materializeStack(
    const std::vector<const std::string*>& stack) {
    std::vector<std::string> result;
    result.reserve(stack.size());
    for (const auto* frame : stack) {
        result.push_back(frame ? *frame : std::string{});
    }
    return result;
}

void populateInterpreterSourceLocations(State& state) {
    state.error.sourceLocations.clear();
    state.error.symbolizedStackTrace.clear();
    state.error.sourceLocations.reserve(state.error.stackTrace.size());
    state.error.symbolizedStackTrace.reserve(state.error.stackTrace.size());
    for (std::size_t idx = 0; idx < state.error.stackTrace.size(); ++idx) {
        const auto& frameName = state.error.stackTrace[idx];
        StackTraceLocation loc;
        loc.function = frameName;
        if (state.program) {
            if (const auto symbolId = state.program->findFunction(frameName)) {
                loc.symbolId = *symbolId;
                if (const auto* fnLoc = state.program->resolveFunction(*symbolId)) {
                    const auto& dbg = fnLoc->function->debugInfo;
                    const debug::SequencePoint* activeSp = nullptr;
                    if (idx < state.debugFrames.size()) {
                        const auto& dbgFrame =
                            state.debugFrames[state.debugFrames.size() - 1 - idx];
                        if (dbgFrame.function == fnLoc->function && dbgFrame.point) {
                            activeSp = dbgFrame.point;
                        }
                    }
                    if (!activeSp && !dbg.sequencePoints.empty()) {
                        activeSp = &dbg.sequencePoints.front();
                    }
                    if (activeSp && fnLoc->module) {
                        for (const auto& src : fnLoc->module->sourceFiles) {
                            if (src.id == activeSp->range.fileId) {
                                loc.sourcePath = src.path;
                                break;
                            }
                        }
                        loc.sourceLine =
                            static_cast<std::uint32_t>(activeSp->range.start.line + 1);
                        loc.sourceColumn =
                            static_cast<std::uint32_t>(activeSp->range.start.column + 1);
                    }
                }
            }
        }
        state.error.symbolizedStackTrace.push_back(loc.format());
        state.error.sourceLocations.push_back(std::move(loc));
    }
}

void refineInterpreterFrameSourceLocation(
    State& state,
    const ProgramImage::FunctionLocation& location,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    std::uint32_t span,
    bool isTerminator = false) {
    if (state.stack.empty() ||
        state.error.sourceLocations.size() < state.stack.size() ||
        state.error.symbolizedStackTrace.size() != state.error.sourceLocations.size()) {
        return;
    }
    const std::size_t traceIdx = state.error.sourceLocations.size() - state.stack.size();
    auto& loc = state.error.sourceLocations[traceIdx];
    if (!location.function || !location.module) return;
    const auto& dbg = location.function->debugInfo;
    if (dbg.sequencePoints.empty()) return;

    const std::uint32_t maxInstIdx =
        instructionIndex + (span > 1 ? span - 1 : 0);
    const debug::SequencePoint* bestSp = nullptr;
    for (const auto& sp : dbg.sequencePoints) {
        if (sp.blockId != blockId) continue;
        if (isTerminator && sp.terminator) {
            bestSp = &sp;
            break;
        }
        if (!sp.terminator && sp.instructionIndex <= maxInstIdx) {
            bestSp = &sp;
        }
    }
    if (!bestSp) {
        bestSp = &dbg.sequencePoints.front();
    }
    for (const auto& src : location.module->sourceFiles) {
        if (src.id == bestSp->range.fileId) {
            loc.sourcePath = src.path;
            break;
        }
    }
    loc.sourceLine = static_cast<std::uint32_t>(bestSp->range.start.line + 1);
    loc.sourceColumn = static_cast<std::uint32_t>(bestSp->range.start.column + 1);
    state.error.symbolizedStackTrace[traceIdx] = loc.format();
}

class ShadowFrameScope {
public:
    ShadowFrameScope(
        ShadowStack& shadowStack,
        const std::vector<Value>* arguments,
        const std::vector<Value>* locals,
        const std::vector<Value>* registers,
        const std::size_t* activeRegisterLimit = nullptr)
        : shadowStack_(shadowStack) {
        shadowStack_.pushFrame(arguments, locals, registers, activeRegisterLimit);
    }
    ~ShadowFrameScope() { shadowStack_.popFrame(); }
    ShadowFrameScope(const ShadowFrameScope&) = delete;
    ShadowFrameScope& operator=(const ShadowFrameScope&) = delete;

private:
    ShadowStack& shadowStack_;
};

class DebugFrameScope {
public:
    DebugFrameScope(State& state, debug::DebugFrameView view)
        : state_(state) {
        if (state_.debugger) {
            state_.debugFrames.push_back(std::move(view));
            active_ = true;
        }
    }
    ~DebugFrameScope() {
        if (active_) state_.debugFrames.pop_back();
    }
    DebugFrameScope(const DebugFrameScope&) = delete;
    DebugFrameScope& operator=(const DebugFrameScope&) = delete;

private:
    State& state_;
    bool active_ = false;
};

void emitTrace(
    State& state,
    TraceEventKind kind,
    DeterminismOperationId operationId = 0,
    std::string_view operation = {}) {
    if (!state.traceEventsEnabled) return;
    if (state.determinismEventsEnabled) {
        state.determinismEventDigest = accumulateDeterminismEvent(
            state.determinismEventDigest,
            kind,
            state.currentFunctionId,
            operationId,
            state.executed,
            state.stack.size());
        ++state.determinismEventCount;
    }
    if (state.profile) {
        ++state.profileEvents;
        auto& profile = state.profileFunctions[state.currentFunctionId];
        if (profile.function.empty() && !state.stack.empty()) {
            profile.function = *state.stack.back();
        }
        profile.maximumCallDepth = std::max(
            profile.maximumCallDepth, state.stack.size());
        switch (kind) {
        case TraceEventKind::FunctionEnter: ++profile.calls; break;
        case TraceEventKind::FunctionExit: ++profile.returns; break;
        case TraceEventKind::Instruction: ++profile.instructions; break;
        case TraceEventKind::Branch: ++profile.branches; break;
        case TraceEventKind::ExternalCall: ++profile.externalCalls; break;
        case TraceEventKind::GcStep: ++profile.gcSteps; break;
        case TraceEventKind::RuntimeError: ++profile.runtimeErrors; break;
        }
    }
    if (!state.diagnosticEventsEnabled) return;
    TraceEvent event;
    event.kind = kind;
    event.function = state.stack.empty()
        ? std::string{}
        : *state.stack.back();
    event.operation.assign(operation.data(), operation.size());
    event.instructionIndex = state.executed;
    event.callDepth = state.stack.size();
    if (state.trace && *state.trace) (*state.trace)(event);
}

void mergeProfile(State& state) {
    if (!state.profile) return;
    ExecutionProfile profile;
    profile.totalEvents = state.profileEvents;
    profile.functions.reserve(state.profileFunctions.size());
    for (auto& [symbolId, function] : state.profileFunctions) {
        (void)symbolId;
        profile.functions.push_back(std::move(function));
    }
    state.profile->merge(profile);
}

bool notifyDebuggerException(
    State& state,
    ErrorCode code,
    std::string exceptionId,
    std::string description,
    bool isUncaught,
    bool isRuntimeError) {
    if (!state.debugger || state.exceptionNotifiedDebugger ||
        code == ErrorCode::ExecutionTerminated || state.debugFrames.empty()) {
        return true;
    }
    auto& topFrame = state.debugFrames.back();
    if (!topFrame.point && topFrame.function &&
        !topFrame.function->debugInfo.sequencePoints.empty()) {
        topFrame.point = &topFrame.function->debugInfo.sequencePoints.front();
    }
    if (isUncaught || isRuntimeError) {
        state.exceptionNotifiedDebugger = true;
    }
    return state.debugger->onException(
        state.debugFrames,
        state.heap,
        code,
        std::move(exceptionId),
        std::move(description),
        isUncaught,
        isRuntimeError);
}

bool fail(State& state, ErrorCode code, std::string message) {
    state.error.code = code;
    state.error.message = std::move(message);
    state.error.stackTrace = materializeStack(state.stack);
    std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
    populateInterpreterSourceLocations(state);
    emitTrace(state, TraceEventKind::RuntimeError, 0, state.error.message);
    notifyDebuggerException(
        state,
        code,
        std::string(errorCodeName(code)),
        state.error.message,
        true,
        true);
    return false;
}

inline void stepGcIfNeeded(State& state, std::uint64_t steps = 1) {
    if (state.limits.gcWorkBudget == 0 || !state.heap) return;
    if (state.traceEventsEnabled && state.heap->isWallClockCycleActive()) {
        state.heap->finishWallClockCycleForDeterminism(state.shadowStack);
    }
    if (state.heap->phase() == GcPhase::Idle && !state.heap->collectionRequested()) {
        return;
    }
    for (std::uint64_t i = 0; i < steps; ++i) {
        if (state.heap->phase() == GcPhase::Idle && !state.heap->collectionRequested()) {
            break;
        }
        const auto work = state.heap->step(
            state.shadowStack,
            state.limits.gcWorkBudget);
        state.statistics.gcWorkPerformed += work;
        if (work != 0 && state.traceEventsEnabled) {
            if (state.diagnosticEventsEnabled) {
                const auto workText = std::to_string(work);
                emitTrace(state, TraceEventKind::GcStep, work, workText);
            } else {
                emitTrace(state, TraceEventKind::GcStep, work);
            }
        }
    }
}

bool consume(
    State& state,
    DeterminismOperationId operationId,
    std::string_view operation = {}) {
    if (state.executed >= state.limits.instructionBudget) {
        return fail(state, ErrorCode::InstructionBudgetExceeded,
            "instruction budget exceeded");
    }
    ++state.executed;
    if (state.fastAccounting) {
        stepGcIfNeeded(state, 1);
        return true;
    }
    state.statistics.instructionsExecuted = state.executed;
    if (state.traceEventsEnabled && !operation.empty()) {
        if (state.determinismOnly) {
            state.determinismEventDigest = accumulateDeterminismEvent(
                state.determinismEventDigest,
                TraceEventKind::Instruction,
                state.currentFunctionId,
                operationId,
                state.executed,
                state.stack.size());
            ++state.determinismEventCount;
        } else {
            emitTrace(state, TraceEventKind::Instruction,
                operationId, operation);
        }
    }
    stepGcIfNeeded(state, 1);
    return true;
}

bool expectType(
    State& state,
    const Value& value,
    semantic::PrimitiveType type,
    std::string_view context) {
    if (const auto* reference = std::get_if<ObjectRef>(&value)) {
        if (!state.heap || !state.heap->isAlive(*reference)) {
            return fail(state, ErrorCode::InvalidObjectReference,
                std::string(context) + " contains an invalid managed object reference");
        }
    }
    if (valueType(value) != type) {
        return fail(state, ErrorCode::TypeMismatch,
            std::string(context) + " expected '" + semantic::primitiveTypeName(type) +
            "', got '" + semantic::primitiveTypeName(valueType(value)) + "'");
    }
    return true;
}

const semantic::TypeSymbol* typeDescriptor(
    const bytecode::Module& module,
    std::uint32_t index) {
    return index < module.types.size() ? &module.types[index] : nullptr;
}

const semantic::TypeSymbol* typeDescriptorById(
    const bytecode::Module& module,
    semantic::SymbolId id) {
    for (const auto& type : module.types) {
        if (type.id == id) return &type;
    }
    return nullptr;
}

semantic::SymbolId exactTypeId(
    semantic::PrimitiveType type,
    const std::string& typeName) {
    return semantic::isExactType(type) && !typeName.empty()
        ? semantic::stableTypeId(typeName)
        : 0;
}

bool runtimeAssignable(
    const State& state,
    semantic::SymbolId actual,
    semantic::SymbolId expected) noexcept {
    if (actual == expected) return true;
    if (actual == 0) return false;
    if (state.mirror) {
        const auto it = state.mirror->typeDispatch.find(actual);
        if (it != state.mirror->typeDispatch.end()) {
            if (expected == 0) return true;
            for (const auto cand : it->second.assignableTargetIds) {
                if (cand == expected) return true;
            }
            return false;
        }
    }
    if (!state.program) return false;
    if (expected == 0) return state.program->resolveType(actual) != nullptr;
    auto current = actual;
    for (std::size_t depth = 0; current != 0 && depth < 256; ++depth) {
        const auto* found = state.program->resolveType(current);
        if (!found) return false;
        for (const auto& implementation :
             found->interfaceDispatchMaps) {
            if (implementation.interfaceTypeId == expected) return true;
        }
        if (found->baseTypeId == expected) return true;
        current = found->baseTypeId;
    }
    return false;
}

const detail::PrelinkedDispatchTarget* resolvePrelinkedPolymorphicTarget(
    const detail::ExecutionMirror& mirror,
    semantic::SymbolId actualTypeId,
    const detail::PolymorphicCallSiteDesc& site) noexcept {
    if (!site.valid || actualTypeId == 0) return nullptr;
    const auto tIt = mirror.typeDispatch.find(actualTypeId);
    if (tIt == mirror.typeDispatch.end()) return nullptr;
    const auto& meta = tIt->second;
    if (!site.isInterface) {
        if (site.slot >= meta.virtualTargets.size()) return nullptr;
        const auto& target = meta.virtualTargets[site.slot];
        return (target.target && target.execution) ? &target : nullptr;
    }
    const auto& imaps = meta.interfaceMaps;
    if (imaps.size() <= 4) {
        for (const auto& m : imaps) {
            if (m.interfaceTypeId == site.interfaceTypeId) {
                if (site.slot >= m.slots.size()) return nullptr;
                const auto& target = m.slots[site.slot];
                return (target.target && target.execution) ? &target : nullptr;
            }
        }
        return nullptr;
    }
    std::size_t lo = 0;
    std::size_t hi = imaps.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1u);
        if (imaps[mid].interfaceTypeId < site.interfaceTypeId) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < imaps.size() && imaps[lo].interfaceTypeId == site.interfaceTypeId) {
        if (site.slot >= imaps[lo].slots.size()) return nullptr;
        const auto& target = imaps[lo].slots[site.slot];
        return (target.target && target.execution) ? &target : nullptr;
    }
    return nullptr;
}

const semantic::InterfaceDispatchMap* runtimeInterfaceMap(
    const State& state,
    semantic::SymbolId actual,
    semantic::SymbolId interfaceTypeId) noexcept {
    if (!state.program) return nullptr;
    auto current = actual;
    for (std::size_t depth = 0; current != 0 && depth < 256; ++depth) {
        const auto* found = state.program->resolveType(current);
        if (!found) return nullptr;
        for (const auto& implementation :
             found->interfaceDispatchMaps) {
            if (implementation.interfaceTypeId == interfaceTypeId) {
                return &implementation;
            }
        }
        current = found->baseTypeId;
    }
    return nullptr;
}

bool expectObject(
    State& state,
    const Value& value,
    const semantic::TypeSymbol& type,
    bool allowNull,
    const std::string& context) {
    if (std::holds_alternative<NullObject>(value)) {
        if (allowNull) return true;
        return fail(state, ErrorCode::NullReference,
            context + " attempted to dereference null");
    }
    const auto* reference = std::get_if<ObjectRef>(&value);
    if (!reference || reference->kind != ObjectKind::Record) {
        return fail(state, ErrorCode::TypeMismatch,
            context + " expected object '" + semantic::canonicalTypeName(type) + "'");
    }
    if (!state.heap || !state.heap->isAlive(*reference)) {
        return fail(state, ErrorCode::InvalidObjectReference,
            context + " contains an invalid managed object reference");
    }
    const auto actual = state.heap->objectTypeId(*reference);
    if (!actual || !runtimeAssignable(state, *actual, type.id)) {
        return fail(state, ErrorCode::TypeMismatch,
            context + " expected object '" + semantic::canonicalTypeName(type) + "'");
    }
    return true;
}

bool expectSignatureType(
    State& state,
    const Value& value,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::string_view context) {
    if (!expectType(state, value, type, context)) return false;
    if (type == semantic::PrimitiveType::Object) {
        if (std::holds_alternative<NullObject>(value) || typeId == 0) return true;
        const auto* reference = std::get_if<ObjectRef>(&value);
        const auto actual = reference && state.heap
            ? state.heap->objectTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || !runtimeAssignable(state, *actual, typeId)) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong runtime object type");
        }
    } else if (type == semantic::PrimitiveType::Array) {
        if (std::holds_alternative<NullArray>(value) || typeId == 0) return true;
        const auto* reference = std::get_if<ObjectRef>(&value);
        const auto actual = reference && state.heap
            ? state.heap->arrayTypeId(*reference)
            : std::optional<semantic::SymbolId>{};
        if (!actual || *actual != typeId) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong runtime array type");
        }
    } else if (type == semantic::PrimitiveType::Handle && typeId != 0) {
        NativeHandle handle{};
        if (!value.tryGetNativeHandle(handle) ||
            (handle.valid() && handle.typeId != typeId) ||
            (!handle.valid() && handle.typeId != 0 && handle.typeId != typeId)) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong native handle type");
        }
    } else if (type == semantic::PrimitiveType::Struct) {
        const auto actualTypeId = structValueTypeId(value);
        const auto* structure = std::get_if<StructValue>(&value);
        if (!actualTypeId || typeId == 0 || *actualTypeId != typeId ||
            (structure && !structure->valid())) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong struct type");
        }
    } else if (type == semantic::PrimitiveType::Enum) {
        EnumValue enumeration{};
        if (!value.tryGetEnum(enumeration) || typeId == 0 || enumeration.typeId != typeId) {
            return fail(state, ErrorCode::TypeMismatch,
                std::string(context) + " has the wrong enum type");
        }
    }
    return true;
}

semantic::SymbolId typeIdAt(
    const std::vector<semantic::SymbolId>& typeIds,
    std::size_t index) noexcept {
    return index < typeIds.size() ? typeIds[index] : 0;
}

Value defaultValue(
    const bytecode::Module& module,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::size_t depth = 0) {
    switch (type) {
    case semantic::PrimitiveType::Bool: return false;
    case semantic::PrimitiveType::Byte: return ByteValue{};
    case semantic::PrimitiveType::SByte: return SByteValue{};
    case semantic::PrimitiveType::Short: return ShortValue{};
    case semantic::PrimitiveType::UShort: return UShortValue{};
    case semantic::PrimitiveType::Int: return std::int64_t{0};
    case semantic::PrimitiveType::UInt: return UIntValue{};
    case semantic::PrimitiveType::Long: return LongValue{};
    case semantic::PrimitiveType::ULong: return ULongValue{};
    case semantic::PrimitiveType::Float: return FloatValue{};
    case semantic::PrimitiveType::Double: return 0.0;
    case semantic::PrimitiveType::Char: return CharValue{};
    case semantic::PrimitiveType::String: return NullString{};
    case semantic::PrimitiveType::Object: return NullObject{};
    case semantic::PrimitiveType::Array: return NullArray{};
    case semantic::PrimitiveType::Handle: {
        NativeHandle handle;
        handle.typeId = typeId;
        return handle;
    }
    case semantic::PrimitiveType::Enum: return EnumValue{typeId, 0};
    case semantic::PrimitiveType::Struct: {
        if (const auto inlined = makeDefaultInlineStructValue(typeId)) {
            return *inlined;
        }
        if (depth > 64) return StructValue{};
        const auto* descriptor = typeDescriptorById(module, typeId);
        if (!descriptor || descriptor->kind != semantic::TypeKind::Struct) {
            return StructValue{};
        }
        if (descriptor->fields.size() <= StructValue::MaxInlineFields) {
            std::array<Value, StructValue::MaxInlineFields> stackFields{};
            for (std::size_t i = 0; i < descriptor->fields.size(); ++i) {
                const auto& field = descriptor->fields[i];
                stackFields[i] = defaultValue(
                    module, field.type, exactTypeId(field.type, field.typeName), depth + 1);
            }
            return makeStructValueFromFields(
                typeId, ValueSpan(stackFields.data(), descriptor->fields.size()));
        }
        std::vector<Value> fields;
        fields.reserve(descriptor->fields.size());
        for (const auto& field : descriptor->fields) {
            fields.push_back(defaultValue(
                module, field.type, exactTypeId(field.type, field.typeName), depth + 1));
        }
        return makeStructValueFromFields(typeId, std::move(fields));
    }
    case semantic::PrimitiveType::Null: return std::monostate{};
    case semantic::PrimitiveType::Void:
    case semantic::PrimitiveType::Error:
        break;
    }
    return std::monostate{};
}


bool valuesEqual(State& state, const Value& left, const Value& right, bool& equal) {
    if (valueType(left) != valueType(right)) {
        equal = false;
        return true;
    }
    if (valueType(left) == semantic::PrimitiveType::Object ||
        valueType(left) == semantic::PrimitiveType::Array) {
        const auto validate = [&](const Value& value) {
            const auto* reference = std::get_if<ObjectRef>(&value);
            return !reference || (state.heap && state.heap->isAlive(*reference));
        };
        if (!validate(left) || !validate(right)) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "reference equality encountered an invalid managed reference");
        }
        equal = left == right;
        return true;
    }
    if (valueType(left) != semantic::PrimitiveType::String) {
        equal = left == right;
        return true;
    }
    if (std::holds_alternative<NullString>(left) ||
        std::holds_alternative<NullString>(right)) {
        equal = std::holds_alternative<NullString>(left) &&
            std::holds_alternative<NullString>(right);
        return true;
    }
    const auto getText = [&](const Value& value) -> std::optional<std::string_view> {
        if (const auto* text = std::get_if<std::string>(&value)) return *text;
        if (const auto* reference = std::get_if<ObjectRef>(&value)) {
            if (!state.heap) return std::nullopt;
            return state.heap->stringView(*reference);
        }
        return std::nullopt;
    };
    const auto leftText = getText(left);
    const auto rightText = getText(right);
    if (!leftText || !rightText) {
        return fail(state, ErrorCode::InvalidObjectReference,
            "string equality encountered an invalid managed reference");
    }
    equal = *leftText == *rightText;
    return true;
}

bool checkedIntResult(State& state, std::int64_t value, Value& output) {
    constexpr auto minimum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    constexpr auto maximum = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    if (value < minimum || value > maximum) {
        return fail(state, ErrorCode::IntegerOverflow, "integer arithmetic overflow");
    }
    output = value;
    return true;
}

std::int64_t wrapInt32(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(value);
    return bits <= static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max())
        ? static_cast<std::int64_t>(bits)
        : static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32);
}

std::int64_t signedFromBits(std::uint64_t bits) noexcept {
    return bits <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(bits)
        : -1 - static_cast<std::int64_t>(
            std::numeric_limits<std::uint64_t>::max() - bits);
}


bool checkedLongAdd(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left + right};
    return true;
}

bool checkedLongSubtract(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
        (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left - right};
    return true;
}

bool checkedLongMultiply(State& state, std::int64_t left, std::int64_t right, Value& output) {
    if (left == 0 || right == 0) {
        output = LongValue{};
        return true;
    }
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    bool overflow = false;
    if (left > 0) {
        overflow = right > 0 ? left > maximum / right : right < minimum / left;
    } else {
        overflow = right > 0 ? left < minimum / right :
            (left != 0 && right < maximum / left);
    }
    if (overflow) {
        return fail(state, ErrorCode::IntegerOverflow, "long arithmetic overflow");
    }
    output = LongValue{left * right};
    return true;
}

const debug::SequencePoint* findSequencePoint(
    const bytecode::Function& function,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator) {
    for (const auto& point : function.debugInfo.sequencePoints) {
        if (point.blockId == blockId &&
            point.instructionIndex == instructionIndex &&
            point.terminator == terminator) {
            return &point;
        }
    }
    return nullptr;
}

bool debugSequencePoint(
    State& state,
    const bytecode::Module& /*module*/,
    const bytecode::Function& function,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    bool terminator) {
    if (!state.debugger) return true;
    const auto* point = findSequencePoint(
        function, blockId, instructionIndex, terminator);
    if (!point && !state.debugger->hasPendingInterruption()) return true;
    if (point) {
        state.debugFrames.back().point = point;
    }
    if (!state.debugger->onSequencePoint(state.debugFrames, state.heap)) {
        return fail(state, ErrorCode::ExecutionTerminated,
            "execution terminated by debugger");
    }
    return true;
}

bool executeFunction(
    State& state,
    const ProgramImage::FunctionLocation& location,
    const ArgumentSource& arguments,
    Value& result,
    const detail::ExecutionFunction* cachedExecution = nullptr,
    bool argumentsPrevalidated = false);

bool executeCall(
    State& state,
    const bytecode::Module& module,
    const bytecode::Instruction& instruction,
    const ValueWindow& registers,
    Value& result) {
    if (instruction.index >= module.functionReferences.size()) {
        return fail(state, ErrorCode::InvalidProgram, "call reference index is invalid");
    }
    const auto& reference = module.functionReferences[instruction.index];
    const ArgumentSource arguments(registers, instruction.operands);
    for (const auto operand : instruction.operands) {
        if (operand >= registers.size()) {
            return fail(state, ErrorCode::InvalidProgram, "call operand register is invalid");
        }
    }
    const auto cacheIndex = [&] {
        const auto callSite = reinterpret_cast<std::uintptr_t>(&instruction);
        return static_cast<std::size_t>((callSite >> 4U) & 63U);
    };
    const auto invokeCached = [&](const InlineCallCacheEntry& cached)
            -> std::optional<bool> {
        if (cached.callSite != &instruction ||
            !cached.target || !cached.execution) {
            return std::nullopt;
        }
        return executeFunction(
            state, *cached.target, arguments, result, cached.execution, true);
    };
    const auto tryCachedReceiver = [&](ObjectRef receiver)
            -> std::optional<bool> {
        if (!state.heap || !state.heap->isAlive(receiver)) return std::nullopt;
        const auto& cached = (*state.inlineCallCache)[cacheIndex()];
        return cached.receiver == receiver
            ? invokeCached(cached)
            : std::nullopt;
    };
    const auto tryCachedType = [&](semantic::SymbolId receiverTypeId)
            -> std::optional<bool> {
        const auto& cached = (*state.inlineCallCache)[cacheIndex()];
        return cached.receiverTypeId == receiverTypeId
            ? invokeCached(cached)
            : std::nullopt;
    };

    if (arguments.size() != reference.parameterTypes.size()) {
        return fail(state, ErrorCode::InvalidProgram,
            "call argument count does not match its reference");
    }
    auto targetSymbolId = reference.symbolId;
    if (reference.virtualDispatch && reference.interfaceDispatch) {
        return fail(state, ErrorCode::InvalidProgram,
            "call cannot use virtual and interface dispatch together");
    }
    if (reference.virtualDispatch) {
        if (reference.virtualSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            arguments.empty() ||
            reference.parameterTypes.empty() ||
            reference.parameterTypes.front() !=
                semantic::PrimitiveType::Object) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call has an invalid receiver contract");
        }
        if (std::holds_alternative<NullObject>(arguments.front())) {
            return fail(state, ErrorCode::NullReference,
                "virtual call attempted to dereference null");
        }
        const auto* receiver = std::get_if<ObjectRef>(&arguments.front());
        if (receiver) {
            if (const auto cached = tryCachedReceiver(*receiver)) {
                return *cached;
            }
        }
        const auto actualTypeId = receiver && state.heap
            ? state.heap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        if (!actualTypeId || !state.program) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "virtual call receiver has no runtime type descriptor");
        }
        if (const auto cached = tryCachedType(*actualTypeId)) {
            return *cached;
        }
        if (state.mirror) {
            detail::PolymorphicCallSiteDesc desc{};
            desc.valid = true;
            desc.isInterface = false;
            desc.slot = reference.virtualSlot;
            if (const auto* pre = resolvePrelinkedPolymorphicTarget(*state.mirror, *actualTypeId, desc)) {
                (*state.inlineCallCache)[cacheIndex()] = {
                    &instruction, *receiver, *actualTypeId, pre->target, pre->execution};
                return executeFunction(state, *pre->target, arguments, result, pre->execution, true);
            }
        }
        const auto* actualType = state.program->resolveType(*actualTypeId);
        if (!actualType ||
            reference.virtualSlot >=
                actualType->virtualDispatchTable.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call slot is outside the runtime dispatch table");
        }
        targetSymbolId =
            actualType->virtualDispatchTable[reference.virtualSlot];
        if (targetSymbolId == 0) {
            return fail(state, ErrorCode::InvalidProgram,
                "virtual call reached an abstract runtime slot");
        }
    } else if (reference.interfaceDispatch) {
        if (reference.interfaceTypeId == 0 ||
            reference.interfaceSlot ==
                std::numeric_limits<std::uint32_t>::max() ||
            arguments.empty() ||
            reference.parameterTypes.empty() ||
            reference.parameterTypes.front() !=
                semantic::PrimitiveType::Object ||
            typeIdAt(reference.parameterTypeIds, 0) !=
                reference.interfaceTypeId) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call has an invalid receiver contract");
        }
        if (std::holds_alternative<NullObject>(arguments.front())) {
            return fail(state, ErrorCode::NullReference,
                "interface call attempted to dereference null");
        }
        const auto* receiver = std::get_if<ObjectRef>(&arguments.front());
        if (receiver) {
            if (const auto cached = tryCachedReceiver(*receiver)) {
                return *cached;
            }
        }
        const auto actualTypeId = receiver && state.heap
            ? state.heap->objectTypeId(*receiver)
            : std::optional<semantic::SymbolId>{};
        if (!actualTypeId) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "interface call receiver has no runtime type descriptor");
        }
        if (const auto cached = tryCachedType(*actualTypeId)) {
            return *cached;
        }
        if (state.mirror) {
            detail::PolymorphicCallSiteDesc desc{};
            desc.valid = true;
            desc.isInterface = true;
            desc.interfaceTypeId = reference.interfaceTypeId;
            desc.slot = reference.interfaceSlot;
            if (const auto* pre = resolvePrelinkedPolymorphicTarget(*state.mirror, *actualTypeId, desc)) {
                (*state.inlineCallCache)[cacheIndex()] = {
                    &instruction, *receiver, *actualTypeId, pre->target, pre->execution};
                return executeFunction(state, *pre->target, arguments, result, pre->execution, true);
            }
        }
        const auto* implementation = runtimeInterfaceMap(
            state, *actualTypeId, reference.interfaceTypeId);
        if (!implementation ||
            reference.interfaceSlot >= implementation->slots.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call slot is outside the runtime dispatch map");
        }
        targetSymbolId = implementation->slots[reference.interfaceSlot];
        if (targetSymbolId == 0) {
            return fail(state, ErrorCode::InvalidProgram,
                "interface call reached an unimplemented runtime slot");
        }
    }

    const auto* found = state.program
        ? state.program->resolveFunction(targetSymbolId)
        : nullptr;
    if (found) {
        const auto* execution = detail::findExecutionFunction(
            detail::executionMirror(*state.program), *found->function);
        if ((reference.virtualDispatch || reference.interfaceDispatch) &&
            execution) {
            const auto* receiver = std::get_if<ObjectRef>(&arguments.front());
            const auto receiverTypeId = receiver && state.heap
                ? state.heap->objectTypeId(*receiver)
                : std::optional<semantic::SymbolId>{};
            if (receiverTypeId) {
                (*state.inlineCallCache)[cacheIndex()] = {
                    &instruction, *receiver, *receiverTypeId, found, execution};
            }
        }
        return executeFunction(
            state, *found, arguments, result, execution);
    }
    if (reference.virtualDispatch || reference.interfaceDispatch) {
        return fail(state, ErrorCode::InvalidProgram,
            "runtime dispatch target function is unavailable");
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!expectSignatureType(
                state,
                arguments[index],
                reference.parameterTypes[index],
                typeIdAt(reference.parameterTypeIds, index),
                "call argument")) {
            return false;
        }
    }
    std::array<Value, 8> stackArguments{};
    std::vector<Value> heapArguments;
    ValueSpan argSpan;
    if (arguments.empty()) {
        argSpan = ValueSpan(nullptr, 0);
    } else if (!arguments.indices) {
        argSpan = ValueSpan(
            arguments.values->data() + arguments.offset,
            arguments.count);
    } else {
        bool contiguous = true;
        const auto firstIndex = arguments.indices[0];
        for (std::size_t index = 1; index < arguments.count; ++index) {
            if (arguments.indices[index] != firstIndex + index) {
                contiguous = false;
                break;
            }
        }
        if (contiguous) {
            argSpan = ValueSpan(
                arguments.values->data() + arguments.offset + firstIndex,
                arguments.count);
        } else if (arguments.count <= stackArguments.size()) {
            for (std::size_t index = 0; index < arguments.count; ++index) {
                stackArguments[index] = arguments[index];
            }
            argSpan = ValueSpan(stackArguments.data(), arguments.count);
        } else {
            heapArguments.reserve(arguments.count);
            for (std::size_t index = 0; index < arguments.count; ++index) {
                heapArguments.push_back(arguments[index]);
            }
            argSpan = ValueSpan(heapArguments);
        }
    }
    if (reference.name.rfind("$intrinsic::Async.__", 0) == 0) {
        std::string intrinsicError;
        if (!tryExecuteAsyncIntrinsic(
                reference.name.substr(12u),
                argSpan,
                state.asyncRuntime,
                state.heap,
                result,
                intrinsicError) ||
            !intrinsicError.empty()) {
            const auto code = intrinsicError.find("null") != std::string::npos
                ? ErrorCode::NullReference
                : ErrorCode::InvalidProgram;
            return fail(state, code, intrinsicError);
        }
        return true;
    }
    if (reference.name.rfind("$intrinsic::", 0) == 0) {
        std::string intrinsicError;
        if (!tryExecuteRuntimeIntrinsic(
                reference.name, argSpan, state.heap, result, intrinsicError)) {
            const auto code = intrinsicError.find("null") != std::string::npos
                ? ErrorCode::NullReference
                : ErrorCode::InvalidProgram;
            return fail(state, code, intrinsicError);
        }
        return true;
    }
    RuntimeError externalError;
    ++state.statistics.externalCalls;
    emitTrace(state, TraceEventKind::ExternalCall,
        reference.symbolId, reference.name);
    std::optional<Value> externalResult;
    const ExternalFunction* fallback =
        state.externalResolver && *state.externalResolver
        ? state.externalResolver
        : nullptr;
    if (!state.determinism.invokeExternal(
            state.bindings,
            fallback,
            reference,
            argSpan,
            externalResult,
            externalError)) {
        state.error = std::move(externalError);
        if (state.error.code == ErrorCode::None) {
            state.error.code = ErrorCode::ExternalFunctionUnresolved;
            state.error.message = "external function '" + reference.name + "' failed";
        }
        state.error.stackTrace = materializeStack(state.stack);
        std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
        populateInterpreterSourceLocations(state);
        notifyDebuggerException(
            state,
            state.error.code,
            std::string(errorCodeName(state.error.code)),
            state.error.message,
            true,
            true);
        return false;
    }
    if (!expectSignatureType(
            state,
            *externalResult,
            reference.returnType,
            reference.returnTypeId,
            "external return value")) {
        return false;
    }
    result = std::move(*externalResult);
    return true;
}

const std::shared_ptr<const std::vector<std::size_t>>& delegateReferenceFields() {
    static const auto refs = std::make_shared<const std::vector<std::size_t>>(
        std::initializer_list<std::size_t>{0u, 7u});
    return refs;
}

std::int64_t delegateWord(std::uint64_t value, bool high) noexcept {
    return static_cast<std::int64_t>(
        static_cast<std::uint32_t>(high ? value >> 32u : value));
}

bool readDelegateIdFromFields(
    const Value* fields,
    std::size_t lowIndex,
    semantic::SymbolId& outId) noexcept {
    std::int64_t lo = 0, hi = 0;
    if (!fields[lowIndex].tryGetInt64(lo) ||
        !fields[lowIndex + 1u].tryGetInt64(hi) ||
        lo < 0 || hi < 0 ||
        lo > std::numeric_limits<std::uint32_t>::max() ||
        hi > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    outId = static_cast<std::uint64_t>(static_cast<std::uint32_t>(lo)) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32u);
    return true;
}

bool readDelegateId(
    State& state,
    ObjectRef reference,
    std::size_t lowIndex,
    semantic::SymbolId& value,
    const char* context) {
    ObjectFastView view{};
    if (state.heap && state.heap->tryBindObjectFastView(reference, view) &&
        lowIndex + 1u < view.fieldCount) {
        if (!readDelegateIdFromFields(view.fields, lowIndex, value)) {
            return fail(state, ErrorCode::InvalidProgram, context);
        }
        return true;
    }
    const auto low = state.heap->fieldGet(reference, lowIndex);
    const auto high = state.heap->fieldGet(reference, lowIndex + 1u);
    const auto* lowValue = low ? std::get_if<std::int64_t>(&*low) : nullptr;
    const auto* highValue = high ? std::get_if<std::int64_t>(&*high) : nullptr;
    if (!lowValue || !highValue || *lowValue < 0 || *highValue < 0 ||
        *lowValue > std::numeric_limits<std::uint32_t>::max() ||
        *highValue > std::numeric_limits<std::uint32_t>::max()) {
        return fail(state, ErrorCode::InvalidProgram, context);
    }
    value = static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(*lowValue)) |
        (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(*highValue)) << 32u);
    return true;
}

bool flattenDelegate(
    State& state,
    semantic::SymbolId delegateTypeId,
    const Value& value,
    std::vector<Value>& leaves) {
    if (std::holds_alternative<NullObject>(value)) return true;
    if (!state.heap) {
        return fail(state, ErrorCode::InvalidProgram,
            "delegate invocation has no heap");
    }
    std::array<Value, 64> inlineStack{};
    std::size_t inlineTop = 0;
    std::vector<Value> heapStack;
    inlineStack[inlineTop++] = value;
    std::size_t visitedNodes = 0;
    while (inlineTop > 0 || !heapStack.empty()) {
        Value cur;
        if (!heapStack.empty()) {
            cur = heapStack.back();
            heapStack.pop_back();
        } else {
            cur = inlineStack[--inlineTop];
        }
        if (std::holds_alternative<NullObject>(cur)) continue;
        if (++visitedNodes > 65536u) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation list is too deep");
        }
        const auto* reference = std::get_if<ObjectRef>(&cur);
        ObjectFastView view{};
        if (!reference ||
            !state.heap->tryBindObjectFastView(*reference, view) ||
            view.typeId != delegateTypeId ||
            view.fieldCount != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate invocation list contains an incompatible value");
        }
        std::int64_t flags = -1;
        if (!view.fields[3].tryGetInt64(flags) || flags < 0 || flags > 4) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation-list metadata is corrupt");
        }
        if (flags != 4) {
            leaves.push_back(cur);
            continue;
        }
        const Value left = view.fields[0];
        const Value right = view.fields[7];
        // Push right first so left is popped first (left-to-right order)
        if (!heapStack.empty() || inlineTop + 2u > inlineStack.size()) {
            heapStack.push_back(right);
            heapStack.push_back(left);
        } else {
            inlineStack[inlineTop++] = right;
            inlineStack[inlineTop++] = left;
        }
    }
    return true;
}

bool sameDelegateLeaf(
    State& state,
    const Value& left,
    const Value& right) {
    const auto* leftRef = std::get_if<ObjectRef>(&left);
    const auto* rightRef = std::get_if<ObjectRef>(&right);
    if (!leftRef || !rightRef || !state.heap) return false;
    if (*leftRef == *rightRef && state.heap->isAlive(*leftRef)) return true;
    ObjectFastView lv{}, rv{};
    if (state.heap->tryBindObjectFastView(*leftRef, lv) &&
        state.heap->tryBindObjectFastView(*rightRef, rv) &&
        lv.fieldCount >= 7u && rv.fieldCount >= 7u) {
        for (std::size_t index = 0; index < 7u; ++index) {
            if (!(lv.fields[index] == rv.fields[index])) return false;
        }
        return true;
    }
    for (std::size_t index = 0; index < 7u; ++index) {
        const auto a = state.heap->fieldGet(*leftRef, index);
        const auto b = state.heap->fieldGet(*rightRef, index);
        if (!a || !b || !(*a == *b)) return false;
    }
    return true;
}

bool allocateCombineDelegateNode(
    State& state,
    semantic::SymbolId delegateTypeId,
    const Value& left,
    const Value& right,
    Value& outNode,
    bool* outSlotsMoved = nullptr) {
    const std::array<Value, 8> fields{{
        left,
        std::int64_t{0},
        std::int64_t{0},
        std::int64_t{4},
        std::int64_t{0},
        std::int64_t{0},
        std::int64_t{0},
        right,
    }};
    RuntimeError error;
    bool localMoved = false;
    const auto allocated = state.heap->allocateObjectFromTemplate(
        delegateTypeId,
        fields.data(),
        fields.size(),
        delegateReferenceFields(),
        &error,
        &localMoved);
    if (localMoved && outSlotsMoved) *outSlotsMoved = true;
    if (!allocated) {
        return fail(state,
            error.code == ErrorCode::None
                ? ErrorCode::OutOfMemory
                : error.code,
            error.message.empty()
                ? "delegate-list allocation failed"
                : error.message);
    }
    outNode = *allocated;
    return true;
}

bool buildDelegateList(
    State& state,
    semantic::SymbolId delegateTypeId,
    const std::vector<Value>& leaves,
    Value& result,
    bool* outSlotsMoved = nullptr) {
    if (leaves.empty()) {
        result = NullObject{};
        return true;
    }
    result = leaves.front();
    for (std::size_t index = 1; index < leaves.size(); ++index) {
        Value nextNode;
        if (!allocateCombineDelegateNode(
                state, delegateTypeId, result, leaves[index], nextNode, outSlotsMoved)) {
            return false;
        }
        result = nextNode;
    }
    return true;
}

bool combineOrRemoveDelegateFastImpl(
    State& state,
    semantic::SymbolId delegateTypeId,
    const Value& left,
    const Value& right,
    bool isRemove,
    Value& result,
    bool* outSlotsMoved = nullptr) {
    if (!state.heap) {
        return fail(state, ErrorCode::InvalidProgram, "delegate combination has no heap");
    }
    const bool leftNull = (left.rawTag() == ValueTag::NullObject);
    const bool rightNull = (right.rawTag() == ValueTag::NullObject);
    ObjectFastView leftView{}, rightView{};
    std::int64_t leftFlags = -1, rightFlags = -1;

    if (!leftNull) {
        if (left.rawTag() != ValueTag::ObjectRef ||
            !state.heap->tryBindObjectFastView(left.u_.objectRef, leftView) ||
            leftView.typeId != delegateTypeId ||
            leftView.fieldCount != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate combination left operand has incompatible type");
        }
        if (!leftView.fields[3].tryGetInt64(leftFlags) || leftFlags < 0 || leftFlags > 4) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation-list metadata is corrupt");
        }
    }
    if (!rightNull) {
        if (right.rawTag() != ValueTag::ObjectRef ||
            !state.heap->tryBindObjectFastView(right.u_.objectRef, rightView) ||
            rightView.typeId != delegateTypeId ||
            rightView.fieldCount != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate combination right operand has incompatible type");
        }
        if (!rightView.fields[3].tryGetInt64(rightFlags) || rightFlags < 0 || rightFlags > 4) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation-list metadata is corrupt");
        }
    }

    if (!isRemove) {
        if (leftNull && rightNull) {
            result = NullObject{};
            return true;
        }
        if (leftNull) {
            if (rightFlags != 4) {
                result = right;
                return true;
            }
            std::vector<Value> rLeaves;
            if (!flattenDelegate(state, delegateTypeId, right, rLeaves)) return false;
            result = right;
            return true;
        }
        if (rightNull) {
            if (leftFlags != 4) {
                result = left;
                return true;
            }
            std::vector<Value> lLeaves;
            if (!flattenDelegate(state, delegateTypeId, left, lLeaves)) return false;
            result = left;
            return true;
        }
        if (leftFlags == 4) {
            const auto checkChild = [&](const Value& child) -> bool {
                if (child.rawTag() == ValueTag::NullObject) return true;
                ObjectFastView cv{};
                std::int64_t cf = -1;
                if (child.rawTag() != ValueTag::ObjectRef ||
                    !state.heap->tryBindObjectFastView(child.u_.objectRef, cv) ||
                    cv.typeId != delegateTypeId ||
                    cv.fieldCount != 8u) {
                    return fail(state, ErrorCode::TypeMismatch,
                        "delegate invocation list contains an incompatible value");
                }
                if (!cv.fields[3].tryGetInt64(cf) || cf < 0 || cf > 4) {
                    return fail(state, ErrorCode::InvalidProgram,
                        "delegate invocation-list metadata is corrupt");
                }
                return true;
            };
            if (!checkChild(leftView.fields[0]) || !checkChild(leftView.fields[7])) {
                return false;
            }
        }
        // O(1) single-node append when appending a single leaf delegate (the 99.9% `+=` path!)
        if (rightFlags != 4) {
            return allocateCombineDelegateNode(
                state, delegateTypeId, left, right, result, outSlotsMoved);
        }
        // Appending a multicast right subtree onto left in O(|right|) allocations
        std::vector<Value> rightLeaves;
        if (!flattenDelegate(state, delegateTypeId, right, rightLeaves)) return false;
        Value cur = left;
        for (const auto& rLeaf : rightLeaves) {
            Value nextNode;
            if (!allocateCombineDelegateNode(
                    state, delegateTypeId, cur, rLeaf, nextNode, outSlotsMoved)) {
                return false;
            }
            cur = nextNode;
        }
        result = cur;
        return true;
    }

    // RemoveDelegate (`-=`)
    if (leftNull) {
        result = Value{NullObject{}};
        return true;
    }
    if (rightNull) {
        if (leftFlags == 4) {
            std::vector<Value> lLeaves;
            if (!flattenDelegate(state, delegateTypeId, left, lLeaves)) return false;
        }
        result = left;
        return true;
    }
    if (leftFlags != 4 && rightFlags != 4) {
        if (sameDelegateLeaf(state, left, right)) {
            result = NullObject{};
        } else {
            result = left;
        }
        return true;
    }
    std::vector<Value> leftLeaves;
    std::vector<Value> rightLeaves;
    if (!flattenDelegate(state, delegateTypeId, left, leftLeaves) ||
        !flattenDelegate(state, delegateTypeId, right, rightLeaves)) {
        return false;
    }
    if (!rightLeaves.empty() && rightLeaves.size() <= leftLeaves.size()) {
        std::optional<std::size_t> removeAt;
        for (std::size_t start = 0;
             start + rightLeaves.size() <= leftLeaves.size();
             ++start) {
            bool matches = true;
            for (std::size_t index = 0; index < rightLeaves.size(); ++index) {
                if (!sameDelegateLeaf(state, leftLeaves[start + index], rightLeaves[index])) {
                    matches = false;
                    break;
                }
            }
            if (matches) removeAt = start;
        }
        if (!removeAt) {
            result = left;
            return true;
        }
        leftLeaves.erase(
            leftLeaves.begin() + static_cast<std::ptrdiff_t>(*removeAt),
            leftLeaves.begin() + static_cast<std::ptrdiff_t>(*removeAt + rightLeaves.size()));
    } else {
        result = left;
        return true;
    }
    return buildDelegateList(state, delegateTypeId, leftLeaves, result, outSlotsMoved);
}

bool resolveDelegateLeafFast(
    State& state,
    const detail::ExecutionMirror& mirror,
    ObjectRef leafRef,
    const ObjectFastView& view,
    std::size_t expectedUserArgs,
    DelegateCallSiteCache& siteCache,
    ResolvedDelegateLeaf& outLeaf) noexcept {
    if (view.fieldCount != 8u || !state.program || !state.heap) return false;
    std::int64_t flags = -1;
    std::int64_t slot = -1;
    if (!view.fields[3].tryGetInt64(flags) ||
        !view.fields[4].tryGetInt64(slot) ||
        flags < 0 || flags > 2 ||
        slot < 0 || slot > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    semantic::SymbolId targetSymId = 0;
    if (!readDelegateIdFromFields(view.fields, 1u, targetSymId)) {
        return false;
    }
    const Value& targetVal = view.fields[0];
    const bool hasRecv = (targetVal.rawTag() != ValueTag::NullObject);
    ObjectRef recvRef{};
    if (hasRecv) {
        if (targetVal.rawTag() == ValueTag::ObjectRef) {
            recvRef = targetVal.u_.objectRef;
            if (!state.heap->isAlive(recvRef)) return false;
        }
    }

    const ProgramImage::FunctionLocation* targetLoc = nullptr;
    const detail::ExecutionFunction* calleeExec = nullptr;

    if (flags != 0) {
        if (!hasRecv || targetVal.rawTag() != ValueTag::ObjectRef) return false;
        const semantic::SymbolId recvTid = state.heap->objectTypeIdFast(recvRef);
        if (recvTid == 0) return false;
        detail::PolymorphicCallSiteDesc polyDesc{};
        polyDesc.slot = static_cast<std::uint32_t>(slot);
        polyDesc.isInterface = (flags == 2);
        if (polyDesc.isInterface) {
            if (!readDelegateIdFromFields(view.fields, 5u, polyDesc.interfaceTypeId)) {
                return false;
            }
        }
        polyDesc.valid = true;
        if (const auto* pre = resolvePrelinkedPolymorphicTarget(mirror, recvTid, polyDesc)) {
            targetLoc = pre->target;
            calleeExec = pre->execution;
            targetSymId = pre->symbolId;
        } else {
            return false;
        }
    } else {
        if (siteCache.lastSymId == targetSymId &&
            siteCache.lastSymTarget != nullptr &&
            siteCache.lastSymExec != nullptr) {
            targetLoc = siteCache.lastSymTarget;
            calleeExec = siteCache.lastSymExec;
        } else {
            targetLoc = state.program->resolveFunction(targetSymId);
            if (!targetLoc || !targetLoc->function) return false;
            calleeExec = detail::findExecutionFunction(mirror, *targetLoc->function);
            if (!calleeExec) return false;
            siteCache.lastSymId = targetSymId;
            siteCache.lastSymTarget = targetLoc;
            siteCache.lastSymExec = calleeExec;
        }
    }

    if (!targetLoc || !targetLoc->function || !calleeExec) return false;
    const std::size_t expectedTotalParams = expectedUserArgs + (hasRecv ? 1u : 0u);
    if (targetLoc->function->parameterTypes.size() != expectedTotalParams) {
        return false;
    }

    outLeaf.leafRef = leafRef;
    outLeaf.receiverRef = recvRef;
    outLeaf.receiverVal = targetVal;
    outLeaf.target = targetLoc;
    outLeaf.execution = calleeExec;
    outLeaf.hasBoundReceiver = hasRecv;
    outLeaf.valid = true;
    return true;
}

bool flattenAndResolveMulticastFast(
    State& state,
    const detail::ExecutionMirror& mirror,
    semantic::SymbolId delegateTypeId,
    ObjectRef rootRef,
    std::size_t expectedUserArgs,
    DelegateCallSiteCache& siteCache,
    std::vector<ResolvedDelegateLeaf>& outLeaves) {
    outLeaves.clear();
    if (!state.heap) return false;

    std::array<ObjectRef, 64> inlineStack{};
    std::size_t inlineTop = 0;
    std::vector<ObjectRef> heapStack;
    inlineStack[inlineTop++] = rootRef;
    std::size_t visitedNodes = 0;

    while (inlineTop > 0 || !heapStack.empty()) {
        ObjectRef curRef{};
        if (!heapStack.empty()) {
            curRef = heapStack.back();
            heapStack.pop_back();
        } else {
            curRef = inlineStack[--inlineTop];
        }
        if (++visitedNodes > 65536u) return false;

        ObjectFastView view{};
        if (!state.heap->tryBindObjectFastView(curRef, view) ||
            view.typeId != delegateTypeId ||
            view.fieldCount != 8u) {
            return false;
        }
        std::int64_t flags = -1;
        if (!view.fields[3].tryGetInt64(flags) || flags < 0 || flags > 4) {
            return false;
        }
        if (flags != 4) {
            ResolvedDelegateLeaf leaf{};
            if (!resolveDelegateLeafFast(
                    state, mirror, curRef, view, expectedUserArgs, siteCache, leaf)) {
                return false;
            }
            outLeaves.push_back(std::move(leaf));
            continue;
        }
        const Value& leftVal = view.fields[0];
        const Value& rightVal = view.fields[7];
        const bool rightObj = (rightVal.rawTag() == ValueTag::ObjectRef);
        const bool leftObj = (leftVal.rawTag() == ValueTag::ObjectRef);
        if ((rightVal.rawTag() != ValueTag::NullObject && !rightObj) ||
            (leftVal.rawTag() != ValueTag::NullObject && !leftObj)) {
            return false;
        }
        if (rightObj) {
            if (!heapStack.empty() || inlineTop + 2u > inlineStack.size()) {
                heapStack.push_back(rightVal.u_.objectRef);
            } else {
                inlineStack[inlineTop++] = rightVal.u_.objectRef;
            }
        }
        if (leftObj) {
            if (!heapStack.empty() || inlineTop + 1u > inlineStack.size()) {
                heapStack.push_back(leftVal.u_.objectRef);
            } else {
                inlineStack[inlineTop++] = leftVal.u_.objectRef;
            }
        }
    }
    return true;
}

#if defined(__GNUC__) || defined(__clang__)
#define RS_QUICK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RS_QUICK_UNLIKELY(x) (x)
#endif

inline bool tryEvaluateQuickIntNoFail(
    detail::QuickOpcode opcode,
    bool checkedArithmetic,
    std::int64_t left,
    std::int64_t right,
    Value& output) noexcept {
    const auto writeChecked = [&](std::int64_t v) noexcept -> bool {
        if (RS_QUICK_UNLIKELY(
                v < std::numeric_limits<std::int32_t>::min() ||
                v > std::numeric_limits<std::int32_t>::max())) {
            return false;
        }
        output.storeInt64(v);
        return true;
    };
    switch (opcode) {
    case detail::QuickOpcode::AddInt:
        if (checkedArithmetic) return writeChecked(left + right);
        output.storeInt64(wrapInt32(left + right));
        return true;
    case detail::QuickOpcode::SubtractInt:
        if (checkedArithmetic) return writeChecked(left - right);
        output.storeInt64(wrapInt32(left - right));
        return true;
    case detail::QuickOpcode::MultiplyInt:
        if (checkedArithmetic) return writeChecked(left * right);
        output.storeInt64(wrapInt32(left * right));
        return true;
    case detail::QuickOpcode::DivideInt:
        if (RS_QUICK_UNLIKELY(right == 0 || (left == std::numeric_limits<std::int32_t>::min() && right == -1))) {
            return false;
        }
        output.storeInt64(left / right);
        return true;
    case detail::QuickOpcode::RemainderInt:
        if (RS_QUICK_UNLIKELY(right == 0)) return false;
        output.storeInt64(
            (left == std::numeric_limits<std::int32_t>::min() && right == -1)
                ? std::int64_t{0}
                : (left % right));
        return true;
    case detail::QuickOpcode::LessInt: output.storeBool(left < right); return true;
    case detail::QuickOpcode::LessOrEqualInt: output.storeBool(left <= right); return true;
    case detail::QuickOpcode::GreaterInt: output.storeBool(left > right); return true;
    case detail::QuickOpcode::GreaterOrEqualInt: output.storeBool(left >= right); return true;
    case detail::QuickOpcode::EqualInt: output.storeBool(left == right); return true;
    case detail::QuickOpcode::NotEqualInt: output.storeBool(left != right); return true;
    default:
        return false;
    }
}

inline bool evaluateQuickIntInPlace(
    State& state,
    detail::QuickOpcode opcode,
    bool checkedArithmetic,
    std::int64_t left,
    std::int64_t right,
    Value& output,
    Value* secondaryStore = nullptr) {
    const auto writeInt = [&](std::int64_t v) noexcept {
        output.storeInt64(v);
        if (secondaryStore) secondaryStore->storeInt64(v);
        return true;
    };
    const auto writeChecked = [&](std::int64_t v) -> bool {
        if (RS_QUICK_UNLIKELY(
                v < std::numeric_limits<std::int32_t>::min() ||
                v > std::numeric_limits<std::int32_t>::max())) {
            return fail(state, ErrorCode::IntegerOverflow, "integer overflow");
        }
        return writeInt(v);
    };
    const auto writeBool = [&](bool b) noexcept {
        output.storeBool(b);
        if (secondaryStore) secondaryStore->storeBool(b);
        return true;
    };
    switch (opcode) {
    case detail::QuickOpcode::AddInt:
        return checkedArithmetic ? writeChecked(left + right) : writeInt(wrapInt32(left + right));
    case detail::QuickOpcode::SubtractInt:
        return checkedArithmetic ? writeChecked(left - right) : writeInt(wrapInt32(left - right));
    case detail::QuickOpcode::MultiplyInt:
        return checkedArithmetic ? writeChecked(left * right) : writeInt(wrapInt32(left * right));
    case detail::QuickOpcode::DivideInt:
        if (RS_QUICK_UNLIKELY(right == 0)) {
            return fail(state, ErrorCode::DivisionByZero, "division by zero");
        }
        if (RS_QUICK_UNLIKELY(left == std::numeric_limits<std::int32_t>::min() && right == -1)) {
            return fail(state, ErrorCode::IntegerOverflow, "integer division overflow");
        }
        return writeInt(left / right);
    case detail::QuickOpcode::RemainderInt:
        if (RS_QUICK_UNLIKELY(right == 0)) {
            return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
        }
        return writeInt(
            (left == std::numeric_limits<std::int32_t>::min() && right == -1)
                ? std::int64_t{0}
                : (left % right));
    case detail::QuickOpcode::LessInt: return writeBool(left < right);
    case detail::QuickOpcode::LessOrEqualInt: return writeBool(left <= right);
    case detail::QuickOpcode::GreaterInt: return writeBool(left > right);
    case detail::QuickOpcode::GreaterOrEqualInt: return writeBool(left >= right);
    case detail::QuickOpcode::EqualInt: return writeBool(left == right);
    case detail::QuickOpcode::NotEqualInt: return writeBool(left != right);
    default:
        return fail(state, ErrorCode::InvalidProgram,
            "invalid quickened bytecode opcode");
    }
}
#undef RS_QUICK_UNLIKELY

bool evaluateQuickInt(
    State& state,
    detail::QuickOpcode opcode,
    bool checkedArithmetic,
    std::int64_t left,
    std::int64_t right,
    Value& output) {
    return evaluateQuickIntInPlace(state, opcode, checkedArithmetic, left, right, output, nullptr);
}

bool executeQuickInstruction(
    State& state,
    const bytecode::Module& module,
    const detail::ExecutionInstruction& decoded,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers) {
    if (!consume(
            state,
            decoded.operationId,
            decoded.opcodeName ? std::string_view(decoded.opcodeName) : std::string_view{})) {
        return false;
    }
    switch (decoded.opcode) {
    case detail::QuickOpcode::LoadParameter:
        registers[decoded.result] = arguments[decoded.auxReg1];
        return true;
    case detail::QuickOpcode::ConstantInt:
        registers[decoded.result] = decoded.integerImmediate;
        return true;
    case detail::QuickOpcode::LoadLocal:
        registers[decoded.result] = locals[decoded.auxReg1];
        return true;
    case detail::QuickOpcode::StoreLocal:
        locals[decoded.auxReg1] = registers[decoded.op0];
        return true;
    case detail::QuickOpcode::CheckNotNullFast: {
        const auto& val = registers[decoded.op0];
        if (const auto* ref = std::get_if<ObjectRef>(&val)) {
            if (state.heap && state.heap->isAlive(*ref)) {
                registers[decoded.result] = val;
                return true;
            }
        }
        if (std::holds_alternative<NullObject>(val)) {
            return fail(state, ErrorCode::NullReference,
                "field access attempted to dereference null");
        }
        return fail(state, ErrorCode::InvalidObjectReference,
            "field access contains an invalid managed object reference");
    }
    case detail::QuickOpcode::LoadElementIntFast: {
        const auto& arrVal = registers[decoded.op0];
        const auto& idxVal = registers[decoded.op1];
        if (const auto* ref = std::get_if<ObjectRef>(&arrVal)) {
            const auto idx = std::get<std::int64_t>(idxVal);
            if (idx >= 0 && state.heap) {
                if (const auto* elem = state.heap->arrayGetFast(
                        *ref, static_cast<std::size_t>(idx))) {
                    registers[decoded.result] = *elem;
                    return true;
                }
            }
            if (!state.heap || !state.heap->arrayLength(*ref)) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    "array element load references an invalid managed array");
            }
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        if (std::holds_alternative<NullArray>(arrVal)) {
            return fail(state, ErrorCode::NullReference,
                "array element load attempted to dereference null");
        }
        return fail(state, ErrorCode::InvalidObjectReference,
            "array element load contains an invalid managed object reference");
    }
    case detail::QuickOpcode::StoreElementIntFast: {
        const auto& arrVal = registers[decoded.op0];
        const auto& idxVal = registers[decoded.op1];
        const auto& storeVal = registers[decoded.op2];
        if (const auto* ref = std::get_if<ObjectRef>(&arrVal)) {
            const auto idx = std::get<std::int64_t>(idxVal);
            if (idx >= 0 && state.heap &&
                state.heap->arraySetPrevalidated(
                    *ref, static_cast<std::size_t>(idx), storeVal, false)) {
                return true;
            }
            if (!state.heap || !state.heap->arrayLength(*ref)) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    "array element store references an invalid managed array");
            }
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        if (std::holds_alternative<NullArray>(arrVal)) {
            return fail(state, ErrorCode::NullReference,
                "array element store attempted to dereference null");
        }
        return fail(state, ErrorCode::InvalidObjectReference,
            "array element store contains an invalid managed object reference");
    }
    case detail::QuickOpcode::CallDirect: {
        if (!decoded.directTarget || !decoded.directExecution) {
            return fail(state, ErrorCode::InvalidProgram,
                "direct call execution cache is missing");
        }
        Value callResult;
        const bool ok = (decoded.op0 == detail::InvalidCompactReg)
            ? executeFunction(
                state, *decoded.directTarget, ArgumentSource(registers),
                callResult, decoded.directExecution, true)
            : (decoded.op1 == detail::InvalidCompactReg)
                ? executeFunction(
                    state, *decoded.directTarget,
                    ArgumentSource(registers, static_cast<bytecode::Register>(decoded.op0)),
                    callResult, decoded.directExecution, true)
                : (decoded.op2 == detail::InvalidCompactReg)
                    ? executeFunction(
                        state, *decoded.directTarget,
                        ArgumentSource(
                            registers,
                            static_cast<bytecode::Register>(decoded.op0),
                            static_cast<bytecode::Register>(decoded.op1)),
                        callResult, decoded.directExecution, true)
                    : executeFunction(
                        state, *decoded.directTarget,
                        ArgumentSource(registers, decoded.bytecode->operands),
                        callResult, decoded.directExecution, true);
        if (!ok) {
            return false;
        }
        if (decoded.result == detail::InvalidCompactReg) return true;
        registers[decoded.result] = std::move(callResult);
        return true;
    }
    case detail::QuickOpcode::CallPolled: {
        const auto& instruction = *decoded.bytecode;
        const auto* receiver = std::get_if<ObjectRef>(&registers[decoded.op0]);
        if (receiver && state.heap && state.heap->isAlive(*receiver)) {
            const auto callSite = reinterpret_cast<std::uintptr_t>(&instruction);
            const auto idx = static_cast<std::size_t>((callSite >> 4U) & 63U);
            const auto& cached = (*state.inlineCallCache)[idx];
            if (cached.callSite == &instruction &&
                cached.receiver == *receiver &&
                cached.target && cached.execution) {
                Value callResult;
                if (!executeFunction(
                        state,
                        *cached.target,
                        (decoded.op1 == detail::InvalidCompactReg
                            ? ArgumentSource(registers, static_cast<bytecode::Register>(decoded.op0))
                            : ArgumentSource(registers, instruction.operands)),
                        callResult,
                        cached.execution,
                        true)) {
                    return false;
                }
                if (decoded.result == detail::InvalidCompactReg) return true;
                registers[decoded.result] = std::move(callResult);
                return true;
            }
        }
        Value callResult;
        if (!executeCall(state, module, instruction, registers, callResult)) {
            return false;
        }
        if (decoded.result == detail::InvalidCompactReg) return true;
        registers[decoded.result] = std::move(callResult);
        return true;
    }
    default:
        break;
    }

    const auto* leftPtr = std::get_if<std::int64_t>(&registers[decoded.op0]);
    const auto* rightPtr = std::get_if<std::int64_t>(&registers[decoded.op1]);
    if (!leftPtr || !rightPtr) {
        return fail(state, ErrorCode::TypeMismatch, "expected int operand in quick instruction");
    }
    Value output;
    if (!evaluateQuickInt(
            state, decoded.opcode, decoded.checkedArithmetic1, *leftPtr, *rightPtr, output)) {
        return false;
    }
    registers[decoded.result] = std::move(output);
    return true;
}

bool executeSuperInstruction(
    State& state,
    const detail::ExecutionInstruction& first,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers) {
    switch (first.superOpcode) {
    case detail::SuperOpcode::LoadParameterStoreLocal: {
        state.executed += 2;
        const auto& val = arguments[first.auxReg1];
        registers[first.result] = val;
        locals[first.storeSlot] = val;
        return true;
    }
    case detail::SuperOpcode::BinaryLocalIntConstant:
    case detail::SuperOpcode::BinaryLocalIntLocal:
    case detail::SuperOpcode::BinaryLocalIntConstantStoreLocal:
    case detail::SuperOpcode::BinaryLocalIntLocalStoreLocal: {
        const auto& leftVal = locals[first.auxReg1];
        registers[first.result] = leftVal;
        const auto* leftPtr = std::get_if<std::int64_t>(&leftVal);
        if (!leftPtr) return fail(state, ErrorCode::TypeMismatch, "expected int local");
        const auto left = *leftPtr;
        std::int64_t right = 0;
        if (first.superOpcode == detail::SuperOpcode::BinaryLocalIntConstant ||
            first.superOpcode == detail::SuperOpcode::BinaryLocalIntConstantStoreLocal) {
            right = first.integerImmediate;
            registers[first.auxReg2] = right;
        } else {
            const auto& rightVal = locals[first.op1];
            registers[first.auxReg2] = rightVal;
            const auto* rightPtr = std::get_if<std::int64_t>(&rightVal);
            if (!rightPtr) return fail(state, ErrorCode::TypeMismatch, "expected int local");
            right = *rightPtr;
        }
        state.executed += 3;
        Value output;
        if (!evaluateQuickInt(
                state, first.binaryOpcode1, first.checkedArithmetic1,
                left, right, output)) {
            return false;
        }
        registers[first.superResult] = output;
        if (first.span == 4) {
            ++state.executed;
            locals[first.storeSlot] = std::move(output);
        }
        return true;
    }
    case detail::SuperOpcode::BinaryLocalIntReg:
    case detail::SuperOpcode::BinaryLocalIntRegStoreLocal: {
        const auto& leftVal = locals[first.auxReg1];
        registers[first.result] = leftVal;
        const auto* leftPtr = std::get_if<std::int64_t>(&leftVal);
        const auto* rightPtr = std::get_if<std::int64_t>(&registers[first.op1]);
        if (!leftPtr || !rightPtr) return fail(state, ErrorCode::TypeMismatch, "expected int operand");
        const auto left = *leftPtr;
        const auto right = *rightPtr;
        state.executed += 2;
        Value output;
        if (!evaluateQuickInt(
                state, first.binaryOpcode1, first.checkedArithmetic1,
                left, right, output)) {
            return false;
        }
        registers[first.superResult] = output;
        if (first.span == 3) {
            ++state.executed;
            locals[first.storeSlot] = std::move(output);
        }
        return true;
    }
    case detail::SuperOpcode::BinaryLocalIntRegChainStoreLocal: {
        const auto& leftVal = locals[first.auxReg1];
        registers[first.result] = leftVal;
        const auto* leftPtr = std::get_if<std::int64_t>(&leftVal);
        const auto* right1Ptr = std::get_if<std::int64_t>(&registers[first.op1]);
        if (!leftPtr || !right1Ptr) return fail(state, ErrorCode::TypeMismatch, "expected int operand");
        const auto left = *leftPtr;
        const auto right1 = *right1Ptr;
        state.executed += 2;
        Value midOutput;
        if (!evaluateQuickInt(
                state, first.binaryOpcode1, first.checkedArithmetic1,
                left, right1, midOutput)) {
            return false;
        }
        registers[first.auxReg2] = midOutput;
        const auto midInt = std::get<std::int64_t>(midOutput);
        const auto* right2Ptr = std::get_if<std::int64_t>(&registers[first.op2]);
        if (!right2Ptr) return fail(state, ErrorCode::TypeMismatch, "expected int operand");
        const auto right2 = *right2Ptr;
        ++state.executed;
        Value finalOutput;
        if (!evaluateQuickInt(
                state, first.binaryOpcode2, first.checkedArithmetic2,
                midInt, right2, finalOutput)) {
            return false;
        }
        registers[first.superResult] = finalOutput;
        ++state.executed;
        locals[first.storeSlot] = std::move(finalOutput);
        return true;
    }
    default:
        return false;
    }
}


std::string extractScriptExceptionMessage(const State& state, const Value& excVal) {
    if (!state.heap || !std::holds_alternative<ObjectRef>(excVal)) {
        return "unhandled script exception in async function";
    }
    const ObjectRef ref = std::get<ObjectRef>(excVal);
    const auto tid = state.heap->objectTypeId(ref);
    const auto* typeDesc = (tid && state.program) ? state.program->resolveType(*tid) : nullptr;
    if (typeDesc) {
        for (std::size_t f = 0; f < typeDesc->fields.size(); ++f) {
            if (typeDesc->fields[f].type == semantic::PrimitiveType::String) {
                if (const auto fv = state.heap->fieldGet(ref, f)) {
                    if (!std::holds_alternative<NullString>(*fv) &&
                        !std::holds_alternative<std::monostate>(*fv)) {
                        const auto s = valueToString(*fv, state.heap);
                        if (!s.empty()) return s;
                    }
                }
            }
        }
        if (!typeDesc->name.empty()) return typeDesc->name;
    }
    return "unhandled script exception in async function";
}

void resumeInterpreterCoroutineCallback(
    void* runnerContext,
    CoroutineFrameHandle frameHandle,
    CoroutineFrame& frame,
    AsyncRuntimeContext& asyncContext);

bool suspendInterpreterCoroutine(
    State& state,
    const bytecode::Function& function,
    CoroutineFrameHandle& ioActiveCoroutine,
    const Value* argsPtr,
    std::size_t argCount,
    const Value* localsPtr,
    std::size_t localCount,
    const Value* regsPtr,
    std::size_t regCount,
    std::uint32_t resumeBlockId,
    AwaitableHandle awaitedHandle,
    Value& outAwaitableStruct) {
    if (!state.asyncRuntime) {
        return fail(state, ErrorCode::InvalidProgram, "async suspension requires an AsyncRuntimeContext");
    }
    AwaitableHandle completionHandle{};
    if (!ioActiveCoroutine.valid()) {
        completionHandle = state.asyncRuntime->awaitables().allocatePending(ResumeQueue::Update);
        ioActiveCoroutine = state.asyncRuntime->coroutines().allocateFrame(
            function.symbolId,
            function.returnTypeId,
            completionHandle,
            ResumeQueue::Update);
    } else {
        const auto* existing = state.asyncRuntime->coroutines().resolve(ioActiveCoroutine);
        if (!existing) {
            return fail(state, ErrorCode::InvalidProgram, "active coroutine frame handle is stale");
        }
        if (existing->invalidated) {
            if (awaitedHandle.valid() && !awaitedHandle.isCompletedVoidSentinel()) {
                (void)state.asyncRuntime->awaitables().discard(awaitedHandle);
            }
            return fail(state, ErrorCode::ExecutionTerminated, "coroutine invalidated by hot reload");
        }
        completionHandle = existing->completionHandle;
    }

    std::shared_ptr<const void> ownerCopy{};
    if (state.resumeEnvOwner && *state.resumeEnvOwner) {
        const auto* cur = state.asyncRuntime->coroutines().resolve(ioActiveCoroutine);
        if (!cur || !cur->runnerOwner) {
            ownerCopy = *state.resumeEnvOwner;
        }
    }

    if (!state.asyncRuntime->coroutines().suspendFrame(
            ioActiveCoroutine,
            argsPtr,
            argCount,
            localsPtr,
            localCount,
            regsPtr,
            regCount,
            resumeBlockId,
            awaitedHandle,
            &resumeInterpreterCoroutineCallback,
            state.resumeEnvRaw,
            std::move(ownerCopy))) {
        return fail(state, ErrorCode::InvalidProgram, "failed to suspend coroutine onto awaited handle");
    }
    outAwaitableStruct = makeAwaitableStructValue(function.returnTypeId, completionHandle);
    return true;
}

bool resumeInterpreterAwaitable(
    State& state,
    const Value& awaitableVal,
    bool hasResult,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    Value& outResumedVal) {
    AwaitableHandle awaitedHandle{};
    if (!state.asyncRuntime || !tryExtractAwaitableHandle(awaitableVal, awaitedHandle)) {
        return fail(state, ErrorCode::InvalidProgram, "await.resume local does not hold a valid Awaitable");
    }
    Value payload{};
    RuntimeError faultErr{};
    Value excObj{};
    const AwaitStatus st = state.asyncRuntime->awaitables().getResult(
        awaitedHandle, payload, faultErr, &excObj);
    if (st == AwaitStatus::Succeeded) {
        if (hasResult) {
            if (!expectSignatureType(state, payload, expectedType, expectedTypeId, "await result")) {
                return false;
            }
            outResumedVal = std::move(payload);
        }
        return true;
    }
    if (st == AwaitStatus::Canceled || st == AwaitStatus::Faulted) {
        const auto matRef = materializeAsyncResumeException(
            state.heap, st, faultErr, excObj);
        if (!matRef) {
            return fail(state, ErrorCode::OutOfMemory, "failed to allocate async resume exception");
        }
        state.pendingException = Value(*matRef);
        return false;
    }
    return fail(state, ErrorCode::InvalidProgram, "await.resume encountered non-terminal Awaitable");
}

bool resolveInterpreterCoroutine(
    State& state,
    CoroutineFrameHandle activeCoroutine,
    semantic::SymbolId returnStructTypeId,
    const Value* optionalPayload,
    Value& outWrappedStruct) {
    if (!state.asyncRuntime) {
        return fail(state, ErrorCode::InvalidProgram, "async.resolve requires an AsyncRuntimeContext");
    }
    if (activeCoroutine.valid()) {
        auto* frame = state.asyncRuntime->coroutines().resolve(activeCoroutine);
        if (!frame) {
            return fail(state, ErrorCode::InvalidProgram, "async.resolve coroutine frame is invalid");
        }
        frame->completed = true;
        const Value val = optionalPayload ? *optionalPayload : Value{};
        (void)state.asyncRuntime->awaitables().trySetResult(frame->completionHandle, val);
        outWrappedStruct = makeAwaitableStructValue(returnStructTypeId, frame->completionHandle);
        return true;
    }
    AwaitableHandle h{};
    if (!optionalPayload) {
        h = AwaitableHandle::completedVoid();
    } else {
        h = state.asyncRuntime->awaitables().fromResult(*optionalPayload);
    }
    outWrappedStruct = makeAwaitableStructValue(returnStructTypeId, h);
    return true;
}

bool finishUncaughtInterpreterAsyncException(
    State& state,
    const bytecode::Function& function,
    CoroutineFrameHandle activeCoroutine,
    Value& outResult) {
    if (!state.asyncRuntime || !state.pendingException.has_value()) {
        return false;
    }
    const Value excVal = *state.pendingException;
    state.pendingException.reset();
    state.error = RuntimeError{};

    const bool isCanceled = isAsyncCanceledExceptionObject(state.heap, excVal);
    if (activeCoroutine.valid()) {
        auto* frame = state.asyncRuntime->coroutines().resolve(activeCoroutine);
        if (!frame) return false;
        frame->completed = true;
        if (isCanceled) {
            (void)state.asyncRuntime->awaitables().trySetCanceled(frame->completionHandle);
        } else {
            std::string msg = extractScriptExceptionMessage(state, excVal);
            (void)state.asyncRuntime->awaitables().trySetFaulted(
                frame->completionHandle,
                ErrorCode::ScriptException,
                std::move(msg),
                excVal);
        }
        outResult = makeAwaitableStructValue(function.returnTypeId, frame->completionHandle);
        return true;
    }
    AwaitableHandle h = isCanceled
        ? state.asyncRuntime->awaitables().fromCanceled()
        : state.asyncRuntime->awaitables().fromFaulted(
              ErrorCode::ScriptException,
              extractScriptExceptionMessage(state, excVal),
              excVal);
    outResult = makeAwaitableStructValue(function.returnTypeId, h);
    return true;
}

bool executeInstruction(
    State& state,
    const bytecode::Module& module,
    const bytecode::Function& function,
    const bytecode::Instruction& instruction,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers) {
    if (!consume(
            state,
            instructionOperationId(instruction.opcode),
            bytecode::opcodeName(instruction.opcode))) {
        return false;
    }
    auto operand = [&](std::size_t index) -> const Value* {
        return &registers[instruction.operands[index]];
    };
    auto storeResult = [&](Value value) {
        registers[instruction.result] = std::move(value);
        return true;
    };

    switch (instruction.opcode) {
    case bytecode::Opcode::LoadParameter:
        return storeResult(arguments[instruction.index]);
    case bytecode::Opcode::ConstantInt: {
        const auto resultType = function.registerTypes[instruction.result];
        if (resultType == semantic::PrimitiveType::Long) {
            return storeResult(LongValue{instruction.integerImmediate});
        }
        if (resultType == semantic::PrimitiveType::Enum) {
            return storeResult(EnumValue{
                typeIdAt(function.registerTypeIds, instruction.result),
                instruction.integerImmediate});
        }
        return storeResult(instruction.integerImmediate);
    }
    case bytecode::Opcode::ConstantDouble:
        return storeResult(instruction.doubleImmediate);
    case bytecode::Opcode::ConstantBool:
        return storeResult(instruction.boolImmediate);
    case bytecode::Opcode::ConstantString: {
        if (!state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "string allocation requires a managed heap");
        }
        RuntimeError allocationError;
        const auto reference = state.heap->internString(
            instruction.stringImmediate,
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed string allocation failed"
                    : allocationError.message);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::ConstantNull:
        return storeResult(std::monostate{});
    case bytecode::Opcode::LoadLocal:
        return storeResult(locals[instruction.index]);
    case bytecode::Opcode::StoreLocal: {
        const auto* value = operand(0);
        if (!value) return false;
        locals[instruction.index] = *value;
        return true;
    }
    case bytecode::Opcode::ConvertNullToString: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullString{});
    }
    case bytecode::Opcode::ConvertNullToObject: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullObject{});
    }
    case bytecode::Opcode::ConvertNullToArray: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Null, "conversion")) return false;
        return storeResult(NullArray{});
    }
    case bytecode::Opcode::ConvertIntToLong: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int,
                "numeric conversion")) return false;
        return storeResult(LongValue{std::get<std::int64_t>(*value)});
    }
    case bytecode::Opcode::ConvertIntToDouble: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int,
                "numeric conversion")) return false;
        return storeResult(static_cast<double>(std::get<std::int64_t>(*value)));
    }
    case bytecode::Opcode::ConvertLongToDouble: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Long,
                "numeric conversion")) return false;
        return storeResult(static_cast<double>(std::get<LongValue>(*value).value));
    }
    case bytecode::Opcode::ConvertNumeric: {
        const auto* value = operand(0);
        if (!value || instruction.result == bytecode::InvalidRegister ||
            instruction.result >= function.registerTypes.size()) {
            return fail(
                state, ErrorCode::InvalidProgram,
                "numeric conversion metadata is invalid");
        }
        Value converted;
        if (!tryConvertNumeric(
                *value, function.registerTypes[instruction.result], converted,
                instruction.checkedArithmetic)) {
            return fail(
                state, ErrorCode::IntegerOverflow,
                "numeric conversion overflow");
        }
        return storeResult(std::move(converted));
    }
    case bytecode::Opcode::ConstantTypeId:
        return storeResult(ULongValue{
            static_cast<std::uint64_t>(instruction.integerImmediate)});
    case bytecode::Opcode::IsType:
    case bytecode::Opcode::AsType: {
        const auto* value = operand(0);
        if (!value || instruction.operands.front() >=
                function.registerTypes.size()) {
            return fail(
                state, ErrorCode::InvalidProgram,
                "runtime type operation metadata is invalid");
        }
        const auto sourceType = function.registerTypes[
            instruction.operands.front()];
        const bool nullValue =
            std::holds_alternative<std::monostate>(*value) ||
            std::holds_alternative<NullString>(*value) ||
            std::holds_alternative<NullObject>(*value) ||
            std::holds_alternative<NullArray>(*value);
        bool matches = false;
        if (!nullValue) {
            if (instruction.elementType == semantic::PrimitiveType::Object) {
                const auto* reference = std::get_if<ObjectRef>(value);
                const auto actual = reference && state.heap
                    ? state.heap->objectTypeId(*reference)
                    : std::optional<semantic::SymbolId>{};
                matches = actual && runtimeAssignable(
                    state, *actual, instruction.elementTypeId);
            } else if (instruction.elementType ==
                       semantic::PrimitiveType::Array) {
                const auto* reference = std::get_if<ObjectRef>(value);
                const auto actual = reference && state.heap
                    ? state.heap->arrayTypeId(*reference)
                    : std::optional<semantic::SymbolId>{};
                matches = actual && *actual == instruction.elementTypeId;
            } else {
                matches = sourceType == instruction.elementType;
            }
        }
        if (instruction.opcode == bytecode::Opcode::IsType) {
            return storeResult(matches);
        }
        if (matches) return storeResult(*value);
        if (instruction.elementType == semantic::PrimitiveType::String) {
            return storeResult(NullString{});
        }
        if (instruction.elementType == semantic::PrimitiveType::Array) {
            return storeResult(NullArray{});
        }
        return storeResult(NullObject{});
    }
    case bytecode::Opcode::NewArray: {
        const auto* lengthValue = operand(0);
        if (!lengthValue ||
            !expectType(state, *lengthValue, semantic::PrimitiveType::Int,
                "array length") ||
            !state.heap) {
            return false;
        }
        const auto length = std::get<std::int64_t>(*lengthValue);
        if (length < 0) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array length cannot be negative");
        }
        if (static_cast<std::uint64_t>(length) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return fail(state, ErrorCode::OutOfMemory, "array length is too large");
        }
        if (instruction.result >= function.registerTypeIds.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "array allocation result type metadata is missing");
        }
        RuntimeError allocationError;
        const auto reference = state.heap->allocateTypedArray(
            function.registerTypeIds[instruction.result],
            instruction.elementType,
            instruction.elementTypeId,
            static_cast<std::size_t>(length),
            defaultValue(module, instruction.elementType, instruction.elementTypeId),
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed array allocation failed"
                    : allocationError.message);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::NewObject: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || type->interfaceType || type->delegateType ||
            !state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "object allocation references an invalid class or heap");
        }
        std::vector<Value> fields;
        std::vector<std::size_t> referenceFields;
        fields.reserve(type->fields.size());
        for (std::size_t index = 0; index < type->fields.size(); ++index) {
            const auto fieldType = type->fields[index].type;
            const auto fieldTypeId =
                exactTypeId(fieldType, type->fields[index].typeName);
            fields.push_back(defaultValue(
                module, fieldType, fieldTypeId));
            if (fieldType == semantic::PrimitiveType::String ||
                fieldType == semantic::PrimitiveType::Object ||
                fieldType == semantic::PrimitiveType::Array ||
                (fieldType == semantic::PrimitiveType::Struct &&
                 !makeDefaultInlineStructValue(fieldTypeId).has_value())) {
                referenceFields.push_back(index);
            }
        }
        RuntimeError allocationError;
        const auto reference = state.heap->allocateObject(
            type->id,
            std::move(fields),
            std::move(referenceFields),
            &allocationError);
        if (!reference) {
            return fail(state,
                allocationError.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : allocationError.code,
                allocationError.message.empty()
                    ? "managed object allocation failed"
                    : allocationError.message);
        }
        return storeResult(*reference);
    }
    case bytecode::Opcode::NewStruct: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || type->kind != semantic::TypeKind::Struct) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct allocation references an invalid type descriptor");
        }
        if (const auto inlined = makeDefaultInlineStructValue(type->id)) {
            return storeResult(*inlined);
        }
        if (type->fields.size() <= StructValue::MaxInlineFields) {
            std::array<Value, StructValue::MaxInlineFields> stackFields{};
            for (std::size_t i = 0; i < type->fields.size(); ++i) {
                const auto& field = type->fields[i];
                stackFields[i] = defaultValue(
                    module, field.type, exactTypeId(field.type, field.typeName));
            }
            return storeResult(makeStructValueFromFields(
                type->id, ValueSpan(stackFields.data(), type->fields.size())));
        }
        std::vector<Value> fields;
        fields.reserve(type->fields.size());
        for (const auto& field : type->fields) {
            fields.push_back(defaultValue(
                module, field.type, exactTypeId(field.type, field.typeName)));
        }
        return storeResult(makeStructValueFromFields(type->id, std::move(fields)));
    }
    case bytecode::Opcode::ArrayLength: {
        const auto* receiver = operand(0);
        if (!receiver) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array length attempted to dereference null");
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array length")) {
            return false;
        }
        const auto* reference = std::get_if<ObjectRef>(receiver);
        const auto length = reference && state.heap
            ? state.heap->arrayLength(*reference)
            : std::optional<std::size_t>{};
        if (!length || *length >
                static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array length references an invalid managed array");
        }
        return storeResult(static_cast<std::int64_t>(*length));
    }
    case bytecode::Opcode::LoadElement: {
        const auto* receiver = operand(0);
        const auto* indexValue = operand(1);
        if (!receiver || !indexValue) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array element load attempted to dereference null");
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array element load") ||
            !expectType(state, *indexValue, semantic::PrimitiveType::Int,
                "array index")) {
            return false;
        }
        const auto index = std::get<std::int64_t>(*indexValue);
        const auto reference = std::get<ObjectRef>(*receiver);
        const auto length = state.heap->arrayLength(reference);
        if (!length) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array element load references an invalid managed array");
        }
        if (index < 0 || static_cast<std::uint64_t>(index) >= *length) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        const auto elementType = state.heap->arrayElementType(reference);
        const auto elementTypeId = state.heap->arrayElementTypeId(reference);
        if (!elementType || !elementTypeId ||
            *elementType != instruction.elementType ||
            *elementTypeId != instruction.elementTypeId) {
            return fail(state, ErrorCode::TypeMismatch,
                "array element metadata does not match the bytecode operation");
        }
        const auto value = state.heap->arrayGet(
            reference, static_cast<std::size_t>(index));
        if (!value || !expectSignatureType(
                state, *value, instruction.elementType,
                instruction.elementTypeId, "array element value")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::StoreElement: {
        const auto* receiver = operand(0);
        const auto* indexValue = operand(1);
        const auto* value = operand(2);
        if (!receiver || !indexValue || !value) return false;
        if (std::holds_alternative<NullArray>(*receiver)) {
            return fail(state, ErrorCode::NullReference,
                "array element store attempted to dereference null");
        }
        if (!expectType(state, *receiver, semantic::PrimitiveType::Array,
                "array element store") ||
            !expectType(state, *indexValue, semantic::PrimitiveType::Int,
                "array index") ||
            !expectSignatureType(state, *value, instruction.elementType,
                instruction.elementTypeId, "array element store value")) {
            return false;
        }
        const auto index = std::get<std::int64_t>(*indexValue);
        const auto reference = std::get<ObjectRef>(*receiver);
        const auto length = state.heap->arrayLength(reference);
        if (!length) {
            return fail(state, ErrorCode::InvalidObjectReference,
                "array element store references an invalid managed array");
        }
        if (index < 0 || static_cast<std::uint64_t>(index) >= *length) {
            return fail(state, ErrorCode::IndexOutOfRange,
                "array element index is out of range");
        }
        const auto elementType = state.heap->arrayElementType(reference);
        const auto elementTypeId = state.heap->arrayElementTypeId(reference);
        if (!elementType || !elementTypeId ||
            *elementType != instruction.elementType ||
            *elementTypeId != instruction.elementTypeId) {
            return fail(state, ErrorCode::TypeMismatch,
                "array element metadata does not match the bytecode operation");
        }
        RuntimeError arrayError;
        if (!state.heap->arraySet(
                reference,
                static_cast<std::size_t>(index),
                *value,
                &arrayError,
                instruction.elementType == semantic::PrimitiveType::Object
                    ? ArrayStoreTypePolicy::PrevalidatedAssignableObject
                    : ArrayStoreTypePolicy::Exact)) {
            return fail(state,
                arrayError.code == ErrorCode::None
                    ? ErrorCode::InvalidProgram
                    : arrayError.code,
                arrayError.message.empty()
                    ? "array element store failed"
                    : arrayError.message);
        }
        return true;
    }
    case bytecode::Opcode::CheckNotNull: {
        const auto* value = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!value || !type || !expectObject(state, *value, *type, false, "field access")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::LoadField: {
        const auto* receiver = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !type ||
            instruction.index >= type->fields.size() ||
            !expectObject(state, *receiver, *type, false, "field load")) {
            return false;
        }
        const auto reference = std::get<ObjectRef>(*receiver);
        const auto value = state.heap->fieldGet(reference, instruction.index);
        if (!value) {
            return fail(state, ErrorCode::InvalidProgram,
                "field load references an invalid field slot");
        }
        const auto& field = type->fields[instruction.index];
        if (!expectSignatureType(
                state,
                *value,
                field.type,
                exactTypeId(field.type, field.typeName),
                "field load value")) {
            return false;
        }
        return storeResult(*value);
    }
    case bytecode::Opcode::StoreField: {
        const auto* receiver = operand(0);
        const auto* value = operand(1);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !value || !type ||
            instruction.index >= type->fields.size() ||
            !expectObject(state, *receiver, *type, false, "field store") ||
            !expectSignatureType(
                state,
                *value,
                type->fields[instruction.index].type,
                exactTypeId(
                    type->fields[instruction.index].type,
                    type->fields[instruction.index].typeName),
                "field store value")) {
            return false;
        }
        RuntimeError fieldError;
        if (!state.heap->fieldSet(
                std::get<ObjectRef>(*receiver),
                instruction.index,
                *value,
                &fieldError)) {
            return fail(state,
                fieldError.code == ErrorCode::None
                    ? ErrorCode::InvalidProgram
                    : fieldError.code,
                fieldError.message.empty()
                    ? "field store failed"
                    : fieldError.message);
        }
        return true;
    }
    case bytecode::Opcode::LoadStructField: {
        const auto* receiver = operand(0);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !type || type->kind != semantic::TypeKind::Struct ||
            instruction.index >= type->fields.size() ||
            !expectSignatureType(state, *receiver, semantic::PrimitiveType::Struct,
                type->id, "struct field load")) {
            return false;
        }
        const auto& field = type->fields[instruction.index];
        if (const auto inlinedField = loadInlineStructField(*receiver, instruction.index)) {
            if (!expectSignatureType(state, *inlinedField, field.type,
                    exactTypeId(field.type, field.typeName), "struct field load value")) {
                return false;
            }
            return storeResult(*inlinedField);
        }
        const auto& structure = std::get<StructValue>(*receiver);
        if (!structure.storage || instruction.index >= structure.storage->fields.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct field load references an invalid field slot");
        }
        const auto& value = structure.storage->fields[instruction.index];
        if (!expectSignatureType(state, value, field.type,
                exactTypeId(field.type, field.typeName), "struct field load value")) {
            return false;
        }
        return storeResult(value);
    }
    case bytecode::Opcode::StoreStructField: {
        const auto* receiver = operand(0);
        const auto* value = operand(1);
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!receiver || !value || !type || type->kind != semantic::TypeKind::Struct ||
            instruction.index >= type->fields.size() ||
            !expectSignatureType(state, *receiver, semantic::PrimitiveType::Struct,
                type->id, "struct field store")) {
            return false;
        }
        const auto& field = type->fields[instruction.index];
        if (!expectSignatureType(state, *value, field.type,
                exactTypeId(field.type, field.typeName), "struct field store value")) {
            return false;
        }
        if (const auto updatedInline = storeInlineStructField(*receiver, instruction.index, *value)) {
            return storeResult(*updatedInline);
        }
        const auto& structure = std::get<StructValue>(*receiver);
        if (!structure.storage || instruction.index >= structure.storage->fields.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "struct field store references an invalid field slot");
        }
        auto storage = std::make_shared<StructStorage>(*structure.storage);
        storage->fields[instruction.index] = *value;
        return storeResult(StructValue{type->id, std::move(storage)});
    }
    case bytecode::Opcode::NewDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        if (!type || !type->delegateType || !state.heap ||
            instruction.index >= module.functionReferences.size() ||
            instruction.operands.size() > 1) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate creation has invalid metadata");
        }
        const auto& reference = module.functionReferences[instruction.index];
        Value target = NullObject{};
        if (!instruction.operands.empty()) {
            const auto* value = operand(0);
            if (!value || reference.parameterTypes.empty() ||
                !expectSignatureType(
                    state, *value, reference.parameterTypes.front(),
                    typeIdAt(reference.parameterTypeIds, 0),
                    "delegate target")) {
                return false;
            }
            target = *value;
        } else if (!reference.parameterTypes.empty() &&
                   (reference.virtualDispatch ||
                    reference.interfaceDispatch)) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch target has no receiver");
        }
        std::uint64_t flags = 0;
        if (reference.virtualDispatch) flags |= 1u;
        if (reference.interfaceDispatch) flags |= 2u;
        const std::array<Value, 8> fields{{
            std::move(target),
            delegateWord(reference.symbolId, false),
            delegateWord(reference.symbolId, true),
            static_cast<std::int64_t>(flags),
            static_cast<std::int64_t>(reference.virtualDispatch
                ? reference.virtualSlot
                : reference.interfaceDispatch
                    ? reference.interfaceSlot
                    : 0u),
            delegateWord(reference.interfaceTypeId, false),
            delegateWord(reference.interfaceTypeId, true),
            NullObject{},
        }};
        RuntimeError error;
        const auto allocated = state.heap->allocateObjectFromTemplate(
            type->id, fields.data(), fields.size(), delegateReferenceFields(), &error);
        if (!allocated) {
            return fail(state,
                error.code == ErrorCode::None
                    ? ErrorCode::OutOfMemory
                    : error.code,
                error.message.empty()
                    ? "delegate allocation failed"
                    : error.message);
        }
        return storeResult(*allocated);
    }
    case bytecode::Opcode::CombineDelegate:
    case bytecode::Opcode::RemoveDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        const auto* left = operand(0);
        const auto* right = operand(1);
        if (!type || !type->delegateType || !left || !right ||
            !state.heap ||
            !expectSignatureType(
                state, *left, semantic::PrimitiveType::Object,
                type->id, "delegate combination left operand") ||
            !expectSignatureType(
                state, *right, semantic::PrimitiveType::Object,
                type->id, "delegate combination right operand")) {
            return false;
        }
        Value combined;
        if (!combineOrRemoveDelegateFastImpl(
                state,
                type->id,
                *left,
                *right,
                instruction.opcode == bytecode::Opcode::RemoveDelegate,
                combined)) {
            return false;
        }
        return storeResult(std::move(combined));
    }
    case bytecode::Opcode::InvokeDelegate: {
        const auto* type = typeDescriptor(module, instruction.typeIndex);
        const auto* delegateValue = operand(0);
        if (!type || !type->delegateType || !delegateValue ||
            !state.heap) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation has invalid metadata");
        }
        if (std::holds_alternative<NullObject>(*delegateValue)) {
            return fail(state, ErrorCode::NullReference,
                "delegate invocation attempted to dereference null");
        }
        const auto* delegateRef = std::get_if<ObjectRef>(delegateValue);
        const auto actualType = delegateRef
            ? state.heap->objectTypeId(*delegateRef)
            : std::optional<semantic::SymbolId>{};
        if (!delegateRef || !actualType || *actualType != type->id ||
            state.heap->fieldCount(*delegateRef) != 8u) {
            return fail(state, ErrorCode::TypeMismatch,
                "delegate invocation received an incompatible value");
        }
        const auto rootFlagsValue = state.heap->fieldGet(
            *delegateRef, 3u);
        const auto* rootFlags = rootFlagsValue
            ? std::get_if<std::int64_t>(&*rootFlagsValue)
            : nullptr;
        if (rootFlags && *rootFlags == 4) {
            std::vector<Value> leaves;
            if (!flattenDelegate(
                    state, type->id, *delegateValue, leaves)) {
                return false;
            }
            Value lastResult;
            for (const auto& leaf : leaves) {
                std::vector<Value> nestedRegisterValues;
                nestedRegisterValues.reserve(registers.size());
                for (std::size_t index = 0; index < registers.size(); ++index) {
                    nestedRegisterValues.push_back(registers[index]);
                }
                ValueWindow nestedRegisters{
                    &nestedRegisterValues, 0, nestedRegisterValues.size()};
                nestedRegisters[instruction.operands.front()] = leaf;
                if (!executeInstruction(
                        state, module, function, instruction,
                        arguments, locals, nestedRegisters)) {
                    return false;
                }
                if (instruction.result !=
                    bytecode::InvalidRegister) {
                    lastResult = nestedRegisters[instruction.result];
                }
            }
            if (instruction.result ==
                bytecode::InvalidRegister) {
                return true;
            }
            return storeResult(std::move(lastResult));
        }
        const semantic::FunctionSymbol* invoke = nullptr;
        for (const auto& method : type->methods) {
            if (method.name == "Invoke") {
                invoke = &method;
                break;
            }
        }
        if (!invoke || invoke->parameters.empty() ||
            instruction.operands.size() != invoke->parameters.size()) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate invocation signature is invalid");
        }
        std::vector<Value> delegateArguments;
        delegateArguments.reserve(instruction.operands.size());
        for (std::size_t index = 1;
             index < instruction.operands.size(); ++index) {
            const auto* value = operand(index);
            const auto& parameter = invoke->parameters[index];
            const auto parameterType =
                semantic::storageTypeOf(parameter);
            if (!value || !expectSignatureType(
                    state, *value, parameterType,
                    exactTypeId(
                        parameterType,
                        semantic::storageTypeNameOf(parameter)),
                    "delegate argument")) {
                return false;
            }
            delegateArguments.push_back(*value);
        }
        semantic::SymbolId targetSymbolId = 0;
        if (!readDelegateId(
                state, *delegateRef, 1u, targetSymbolId,
                "delegate callable identity is corrupt")) {
            return false;
        }
        const auto target = state.heap->fieldGet(*delegateRef, 0u);
        const auto flagsValue = state.heap->fieldGet(*delegateRef, 3u);
        const auto slotValue = state.heap->fieldGet(*delegateRef, 4u);
        const auto* flags = flagsValue
            ? std::get_if<std::int64_t>(&*flagsValue)
            : nullptr;
        const auto* slot = slotValue
            ? std::get_if<std::int64_t>(&*slotValue)
            : nullptr;
        if (!target || !flags || !slot || *flags < 0 || *flags > 3 ||
            *slot < 0 ||
            *slot > std::numeric_limits<std::uint32_t>::max()) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch metadata is corrupt");
        }
        const bool virtualDispatch = (*flags & 1) != 0;
        const bool interfaceDispatch = (*flags & 2) != 0;
        if (virtualDispatch && interfaceDispatch) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate dispatch modes conflict");
        }
        if (!std::holds_alternative<NullObject>(*target)) {
            delegateArguments.insert(delegateArguments.begin(), *target);
        }
        if (virtualDispatch || interfaceDispatch) {
            const auto* receiver = delegateArguments.empty()
                ? nullptr
                : std::get_if<ObjectRef>(&delegateArguments.front());
            const auto runtimeType = receiver
                ? state.heap->objectTypeId(*receiver)
                : std::optional<semantic::SymbolId>{};
            if (!runtimeType || !state.program) {
                return fail(state, ErrorCode::InvalidObjectReference,
                    "delegate receiver has no runtime type");
            }
            const auto* foundType = state.program->resolveType(*runtimeType);
            if (!foundType) {
                return fail(state, ErrorCode::InvalidProgram,
                    "delegate receiver descriptor is unavailable");
            }
            const auto dispatchSlot = static_cast<std::uint32_t>(*slot);
            if (virtualDispatch) {
                if (dispatchSlot >=
                    foundType->virtualDispatchTable.size()) {
                    return fail(state, ErrorCode::InvalidProgram,
                        "delegate virtual slot is invalid");
                }
                targetSymbolId =
                    foundType->virtualDispatchTable[dispatchSlot];
            } else {
                semantic::SymbolId interfaceTypeId = 0;
                if (!readDelegateId(
                        state, *delegateRef, 5u, interfaceTypeId,
                        "delegate interface identity is corrupt")) {
                    return false;
                }
                const auto* map = runtimeInterfaceMap(
                    state, *runtimeType, interfaceTypeId);
                if (!map || dispatchSlot >= map->slots.size()) {
                    return fail(state, ErrorCode::InvalidProgram,
                        "delegate interface slot is invalid");
                }
                targetSymbolId = map->slots[dispatchSlot];
            }
        }
        const auto* found = state.program
            ? state.program->resolveFunction(targetSymbolId)
            : nullptr;
        if (!found) {
            return fail(state, ErrorCode::InvalidProgram,
                "delegate target function is unavailable");
        }
        Value delegateResult;
        if (!executeFunction(
                state, *found, delegateArguments, delegateResult)) {
            return false;
        }
        if (instruction.result == bytecode::InvalidRegister) return true;
        return storeResult(std::move(delegateResult));
    }
    case bytecode::Opcode::Call: {
        Value callResult;
        if (!executeCall(state, module, instruction, registers, callResult)) return false;
        if (instruction.result == bytecode::InvalidRegister) return true;
        return storeResult(std::move(callResult));
    }
    case bytecode::Opcode::AsyncEnter:
        return true;
    case bytecode::Opcode::AwaitSuspend: {
        const auto* val = operand(0);
        if (!val || instruction.index >= locals.size() || !state.asyncRuntime) {
            return fail(state, ErrorCode::InvalidProgram, "invalid await.suspend operand or local");
        }
        locals[instruction.index] = *val;
        AwaitableHandle awaitedHandle{};
        if (!tryExtractAwaitableHandle(*val, awaitedHandle) ||
            !state.asyncRuntime->awaitables().isValid(awaitedHandle)) {
            return fail(state, ErrorCode::InvalidProgram, "awaited value does not hold a valid Awaitable handle");
        }
        if (state.asyncRuntime->awaitables().isCompleted(awaitedHandle)) {
            return true;
        }
        if (!state.currentBlockVmCoroutinePtr) {
            return fail(state, ErrorCode::InvalidProgram, "missing coroutine context in block VM");
        }
        Value pendingRet{};
        const Value* argsRaw = arguments.size() == 0 ? nullptr : &arguments.values->at(arguments.offset);
        const Value* locsRaw = locals.size() == 0 ? nullptr : &locals.values->at(locals.offset);
        const Value* regsRaw = registers.size() == 0 ? nullptr : &registers.values->at(registers.offset);
        if (!suspendInterpreterCoroutine(
                state,
                function,
                *state.currentBlockVmCoroutinePtr,
                argsRaw,
                arguments.size(),
                locsRaw,
                locals.size(),
                regsRaw,
                registers.size(),
                static_cast<std::uint32_t>(instruction.activeIntegerImmediate()),
                awaitedHandle,
                pendingRet)) {
            return false;
        }
        state.suspendedReturnValue = std::move(pendingRet);
        state.coroutineSuspendedNow = true;
        return true;
    }
    case bytecode::Opcode::AwaitResume: {
        if (instruction.index >= locals.size()) {
            return fail(state, ErrorCode::InvalidProgram, "invalid await.resume local");
        }
        const Value awaitableVal = locals[instruction.index];
        locals[instruction.index] = Value{};
        Value resumedVal{};
        const bool hasRes = (instruction.result != bytecode::InvalidRegister);
        if (!resumeInterpreterAwaitable(
                state,
                awaitableVal,
                hasRes,
                instruction.elementType,
                instruction.activeElementTypeId(),
                resumedVal)) {
            return false;
        }
        if (!hasRes) return true;
        return storeResult(std::move(resumedVal));
    }
    case bytecode::Opcode::AsyncResolve: {
        const auto* retTypeDesc = typeDescriptor(module, instruction.typeIndex);
        if (!retTypeDesc || !state.currentBlockVmCoroutinePtr) {
            return fail(state, ErrorCode::InvalidProgram, "invalid async.resolve metadata");
        }
        const Value* payloadPtr = instruction.operands.empty() ? nullptr : operand(0);
        Value wrapped{};
        if (!resolveInterpreterCoroutine(
                state,
                *state.currentBlockVmCoroutinePtr,
                retTypeDesc->id,
                payloadPtr,
                wrapped)) {
            return false;
        }
        return storeResult(std::move(wrapped));
    }
    case bytecode::Opcode::NegateInt: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Int, "negation")) return false;
        Value output;
        const auto number = std::get<std::int64_t>(*value);
        if (!instruction.checkedArithmetic &&
            number == std::numeric_limits<std::int32_t>::min()) {
            return storeResult(number);
        }
        if (!checkedIntResult(state, -number, output)) return false;
        return storeResult(std::move(output));
    }
    case bytecode::Opcode::NegateLong: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Long,
                "negation")) return false;
        const auto number = std::get<LongValue>(*value).value;
        if (number == std::numeric_limits<std::int64_t>::min()) {
            if (!instruction.checkedArithmetic) return storeResult(LongValue{number});
            return fail(state, ErrorCode::IntegerOverflow, "long negation overflow");
        }
        return storeResult(LongValue{-number});
    }
    case bytecode::Opcode::NegateDouble: {
        const auto* value = operand(0);
        if (value && valueType(*value) == semantic::PrimitiveType::Float) {
            return storeResult(FloatValue{-std::get<FloatValue>(*value).value});
        }
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Double,
                "negation")) return false;
        return storeResult(-std::get<double>(*value));
    }
    case bytecode::Opcode::LogicalNot: {
        const auto* value = operand(0);
        if (!value || !expectType(state, *value, semantic::PrimitiveType::Bool, "logical not")) return false;
        return storeResult(!std::get<bool>(*value));
    }
    default:
        break;
    }

    const auto* left = operand(0);
    const auto* right = operand(1);
    if (!left || !right) return false;

    if (instruction.opcode == bytecode::Opcode::Equal || instruction.opcode == bytecode::Opcode::NotEqual) {
        bool equal = false;
        if (!valuesEqual(state, *left, *right, equal)) return false;
        return storeResult(instruction.opcode == bytecode::Opcode::Equal ? equal : !equal);
    }
    const auto isIntOpcode =
        instruction.opcode >= bytecode::Opcode::AddInt &&
        instruction.opcode <= bytecode::Opcode::GreaterOrEqualInt;
    if (isIntOpcode && instruction.operands.size() >= 2 &&
        instruction.operands[0] < function.registerTypes.size() &&
        instruction.operands[1] < function.registerTypes.size() &&
        function.registerTypes[instruction.operands[0]] ==
            semantic::PrimitiveType::Int &&
        function.registerTypes[instruction.operands[1]] ==
            semantic::PrimitiveType::Int) {
        const auto* typedLeft = std::get_if<std::int64_t>(left);
        const auto* typedRight = std::get_if<std::int64_t>(right);
        if (!typedLeft || !typedRight) {
            return fail(state, ErrorCode::TypeMismatch,
                "binary operation expected int operands");
        }
        const auto a = *typedLeft;
        const auto b = *typedRight;
        Value intOutput;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddInt:
            if (!instruction.checkedArithmetic) {
                return storeResult(wrapInt32(a + b));
            }
            return checkedIntResult(state, a + b, intOutput) &&
                storeResult(std::move(intOutput));
        case bytecode::Opcode::SubtractInt:
            if (!instruction.checkedArithmetic) {
                return storeResult(wrapInt32(a - b));
            }
            return checkedIntResult(state, a - b, intOutput) &&
                storeResult(std::move(intOutput));
        case bytecode::Opcode::MultiplyInt:
            if (!instruction.checkedArithmetic) {
                return storeResult(wrapInt32(a * b));
            }
            return checkedIntResult(state, a * b, intOutput) &&
                storeResult(std::move(intOutput));
        case bytecode::Opcode::DivideInt:
            if (b == 0) {
                return fail(state, ErrorCode::DivisionByZero,
                    "division by zero");
            }
            if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
                return fail(state, ErrorCode::IntegerOverflow,
                    "integer division overflow");
            }
            return storeResult(a / b);
        case bytecode::Opcode::RemainderInt:
            if (b == 0) {
                return fail(state, ErrorCode::DivisionByZero,
                    "remainder by zero");
            }
            if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
                return storeResult(std::int64_t{0});
            }
            return storeResult(a % b);
        case bytecode::Opcode::LessInt: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualInt: return storeResult(a <= b);
        case bytecode::Opcode::GreaterInt: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualInt: return storeResult(a >= b);
        default: break;
        }
    }
    const auto isLongOpcode = instruction.opcode >= bytecode::Opcode::AddLong &&
        instruction.opcode <= bytecode::Opcode::GreaterOrEqualLong;
    const auto isDoubleOpcode = instruction.opcode >= bytecode::Opcode::AddDouble &&
        instruction.opcode <= bytecode::Opcode::GreaterOrEqualDouble;
    Value output;
    if (isLongOpcode) {
        if (valueType(*left) == semantic::PrimitiveType::ULong) {
            if (!expectType(state, *right, semantic::PrimitiveType::ULong,
                    "binary operation")) return false;
            const auto a = std::get<ULongValue>(*left).value;
            const auto b = std::get<ULongValue>(*right).value;
            switch (instruction.opcode) {
            case bytecode::Opcode::AddLong:
                if (instruction.checkedArithmetic &&
                    b > std::numeric_limits<std::uint64_t>::max() - a)
                    return fail(state, ErrorCode::IntegerOverflow, "ulong addition overflow");
                return storeResult(ULongValue{a + b});
            case bytecode::Opcode::SubtractLong:
                if (instruction.checkedArithmetic && a < b) return fail(state, ErrorCode::IntegerOverflow, "ulong subtraction overflow");
                return storeResult(ULongValue{a - b});
            case bytecode::Opcode::MultiplyLong:
                if (instruction.checkedArithmetic && b != 0 &&
                    a > std::numeric_limits<std::uint64_t>::max() / b)
                    return fail(state, ErrorCode::IntegerOverflow, "ulong multiplication overflow");
                return storeResult(ULongValue{a * b});
            case bytecode::Opcode::DivideLong:
                if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
                return storeResult(ULongValue{a / b});
            case bytecode::Opcode::RemainderLong:
                if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
                return storeResult(ULongValue{a % b});
            case bytecode::Opcode::LessLong: return storeResult(a < b);
            case bytecode::Opcode::LessOrEqualLong: return storeResult(a <= b);
            case bytecode::Opcode::GreaterLong: return storeResult(a > b);
            case bytecode::Opcode::GreaterOrEqualLong: return storeResult(a >= b);
            default: break;
            }
        }
        if (!expectType(state, *left, semantic::PrimitiveType::Long, "binary operation") ||
            !expectType(state, *right, semantic::PrimitiveType::Long, "binary operation")) {
            return false;
        }
        const auto a = std::get<LongValue>(*left).value;
        const auto b = std::get<LongValue>(*right).value;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) +
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongAdd(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::SubtractLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) -
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongSubtract(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::MultiplyLong:
            if (!instruction.checkedArithmetic) {
                return storeResult(LongValue{signedFromBits(
                    static_cast<std::uint64_t>(a) *
                    static_cast<std::uint64_t>(b))});
            }
            return checkedLongMultiply(state, a, b, output) && storeResult(output);
        case bytecode::Opcode::DivideLong:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return fail(state, ErrorCode::IntegerOverflow, "long division overflow");
            }
            return storeResult(LongValue{a / b});
        case bytecode::Opcode::RemainderLong:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
            if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
                return storeResult(LongValue{});
            }
            return storeResult(LongValue{a % b});
        case bytecode::Opcode::LessLong: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualLong: return storeResult(a <= b);
        case bytecode::Opcode::GreaterLong: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualLong: return storeResult(a >= b);
        default: break;
        }
    }
    if (isDoubleOpcode) {
        if (valueType(*left) == semantic::PrimitiveType::Float) {
            if (!expectType(state, *right, semantic::PrimitiveType::Float,
                    "binary operation")) return false;
            const auto a = std::get<FloatValue>(*left).value;
            const auto b = std::get<FloatValue>(*right).value;
            switch (instruction.opcode) {
            case bytecode::Opcode::AddDouble: return storeResult(FloatValue{a + b});
            case bytecode::Opcode::SubtractDouble: return storeResult(FloatValue{a - b});
            case bytecode::Opcode::MultiplyDouble: return storeResult(FloatValue{a * b});
            case bytecode::Opcode::DivideDouble: return storeResult(FloatValue{a / b});
            case bytecode::Opcode::LessDouble: return storeResult(a < b);
            case bytecode::Opcode::LessOrEqualDouble: return storeResult(a <= b);
            case bytecode::Opcode::GreaterDouble: return storeResult(a > b);
            case bytecode::Opcode::GreaterOrEqualDouble: return storeResult(a >= b);
            default: break;
            }
        }
        if (!expectType(state, *left, semantic::PrimitiveType::Double, "binary operation") ||
            !expectType(state, *right, semantic::PrimitiveType::Double, "binary operation")) {
            return false;
        }
        const auto a = std::get<double>(*left);
        const auto b = std::get<double>(*right);
        switch (instruction.opcode) {
        case bytecode::Opcode::AddDouble: return storeResult(a + b);
        case bytecode::Opcode::SubtractDouble: return storeResult(a - b);
        case bytecode::Opcode::MultiplyDouble: return storeResult(a * b);
        case bytecode::Opcode::DivideDouble:
            // Floating-point division follows IEEE 754. Zero divisors produce
            // infinities or NaN rather than an integer-style runtime trap.
            return storeResult(a / b);
        case bytecode::Opcode::LessDouble: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualDouble: return storeResult(a <= b);
        case bytecode::Opcode::GreaterDouble: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualDouble: return storeResult(a >= b);
        default: break;
        }
    }
    if (valueType(*left) == semantic::PrimitiveType::UInt) {
        if (!expectType(state, *right, semantic::PrimitiveType::UInt,
                "binary operation")) return false;
        const auto a = std::get<UIntValue>(*left).value;
        const auto b = std::get<UIntValue>(*right).value;
        switch (instruction.opcode) {
        case bytecode::Opcode::AddInt:
            if (instruction.checkedArithmetic &&
                b > std::numeric_limits<std::uint32_t>::max() - a)
                return fail(state, ErrorCode::IntegerOverflow, "uint addition overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a + b)});
        case bytecode::Opcode::SubtractInt:
            if (instruction.checkedArithmetic && a < b) return fail(state, ErrorCode::IntegerOverflow, "uint subtraction overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a - b)});
        case bytecode::Opcode::MultiplyInt:
            if (instruction.checkedArithmetic && b != 0 &&
                a > std::numeric_limits<std::uint32_t>::max() / b)
                return fail(state, ErrorCode::IntegerOverflow, "uint multiplication overflow");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a * b)});
        case bytecode::Opcode::DivideInt:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a / b)});
        case bytecode::Opcode::RemainderInt:
            if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
            return storeResult(UIntValue{static_cast<std::uint32_t>(a % b)});
        case bytecode::Opcode::LessInt: return storeResult(a < b);
        case bytecode::Opcode::LessOrEqualInt: return storeResult(a <= b);
        case bytecode::Opcode::GreaterInt: return storeResult(a > b);
        case bytecode::Opcode::GreaterOrEqualInt: return storeResult(a >= b);
        default: break;
        }
    }
    if (!expectType(state, *left, semantic::PrimitiveType::Int, "binary operation") ||
        !expectType(state, *right, semantic::PrimitiveType::Int, "binary operation")) return false;
    const auto a = std::get<std::int64_t>(*left);
    const auto b = std::get<std::int64_t>(*right);
    switch (instruction.opcode) {
    case bytecode::Opcode::AddInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a + b));
        return checkedIntResult(state, a + b, output) && storeResult(output);
    case bytecode::Opcode::SubtractInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a - b));
        return checkedIntResult(state, a - b, output) && storeResult(output);
    case bytecode::Opcode::MultiplyInt:
        if (!instruction.checkedArithmetic) return storeResult(wrapInt32(a * b));
        return checkedIntResult(state, a * b, output) && storeResult(output);
    case bytecode::Opcode::DivideInt:
        if (b == 0) return fail(state, ErrorCode::DivisionByZero, "division by zero");
        if (a == std::numeric_limits<std::int32_t>::min() && b == -1)
            return fail(state, ErrorCode::IntegerOverflow, "integer division overflow");
        return storeResult(a / b);
    case bytecode::Opcode::RemainderInt:
        if (b == 0) return fail(state, ErrorCode::DivisionByZero, "remainder by zero");
        if (a == std::numeric_limits<std::int32_t>::min() && b == -1) return storeResult(std::int64_t{0});
        return storeResult(a % b);
    case bytecode::Opcode::LessInt: return storeResult(a < b);
    case bytecode::Opcode::LessOrEqualInt: return storeResult(a <= b);
    case bytecode::Opcode::GreaterInt: return storeResult(a > b);
    case bytecode::Opcode::GreaterOrEqualInt: return storeResult(a >= b);
    default:
        return fail(state, ErrorCode::InvalidProgram, "unsupported bytecode opcode");
    }
}

bool executeFunction(
    State& state,
    const ProgramImage::FunctionLocation& location,
    const ArgumentSource& arguments,
    Value& result,
    const detail::ExecutionFunction* cachedExecution,
    bool argumentsPrevalidated) {
    const auto& function = *location.function;
    const auto* execution = cachedExecution
        ? cachedExecution
        : detail::findExecutionFunction(
            detail::executionMirror(*state.program), function);
    if (!execution || execution->entry == detail::InvalidBlockIndex) {
        return fail(state, ErrorCode::InvalidProgram,
            "function execution mirror is missing");
    }
    if (state.stack.size() >= state.limits.recursionLimit) {
        return fail(state, ErrorCode::RecursionLimitExceeded, "recursion limit exceeded");
    }
    CoroutineFrameHandle activeCoroutine{};
    CoroutineFrame* activeCoroutineFrame = nullptr;
    if (state.resumingFramePtr != nullptr &&
        state.resumingCoroutine.valid() &&
        function.symbolId == state.resumingFramePtr->functionSymbolId) {
        activeCoroutine = state.resumingCoroutine;
        activeCoroutineFrame = state.resumingFramePtr;
        state.resumingCoroutine = CoroutineFrameHandle::invalid();
        state.resumingFramePtr = nullptr;
        argumentsPrevalidated = true;
    }
    if (!argumentsPrevalidated &&
        arguments.size() != function.parameterTypes.size()) {
        return fail(state, ErrorCode::InvalidArguments, "argument count does not match function signature");
    }
    if (!argumentsPrevalidated) {
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            if (!expectSignatureType(
                    state,
                    arguments[index],
                    function.parameterTypes[index],
                    typeIdAt(function.parameterTypeIds, index),
                    "argument")) {
                return false;
            }
        }
    }

    if (state.fastAccounting && execution->directNativeReference != nullptr) {
        const std::uint64_t cost = execution->nativeForwarderInstructionCost;
        if (state.executed > state.limits.instructionBudget ||
            cost > state.limits.instructionBudget - state.executed) {
            state.executed = state.limits.instructionBudget;
            return fail(state, ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        state.executed += cost;
        ++state.statistics.functionCalls;
        ++state.statistics.externalCalls;
        if (state.stack.size() + 1 > state.statistics.maximumCallDepth) {
            state.statistics.maximumCallDepth = state.stack.size() + 1;
        }

        const auto& reference = *execution->directNativeReference;
        std::array<Value, 8> stackArguments{};
        std::vector<Value> heapArguments;
        ValueSpan argSpan;
        if (arguments.empty()) {
            argSpan = ValueSpan(nullptr, 0);
        } else if (!arguments.indices) {
            argSpan = ValueSpan(
                arguments.values->data() + arguments.offset,
                arguments.count);
        } else if (arguments.count == 1) {
            argSpan = ValueSpan(
                arguments.values->data() + arguments.offset + arguments.indices[0],
                1);
        } else {
            bool contiguous = true;
            const auto firstIndex = arguments.indices[0];
            for (std::size_t index = 1; index < arguments.count; ++index) {
                if (arguments.indices[index] != firstIndex + index) {
                    contiguous = false;
                    break;
                }
            }
            if (contiguous) {
                argSpan = ValueSpan(
                    arguments.values->data() + arguments.offset + firstIndex,
                    arguments.count);
            } else if (arguments.count <= stackArguments.size()) {
                for (std::size_t index = 0; index < arguments.count; ++index) {
                    stackArguments[index] = arguments[index];
                }
                argSpan = ValueSpan(stackArguments.data(), arguments.count);
            } else {
                heapArguments.reserve(arguments.count);
                for (std::size_t index = 0; index < arguments.count; ++index) {
                    heapArguments.push_back(arguments[index]);
                }
                argSpan = ValueSpan(heapArguments);
            }
        }

        RuntimeError externalError;
        std::optional<Value> externalResult;
        const ExternalFunction* fallback =
            state.externalResolver && *state.externalResolver
            ? state.externalResolver
            : nullptr;
        if (!state.determinism.invokeExternal(
                state.bindings,
                fallback,
                reference,
                argSpan,
                externalResult,
                externalError)) {
            state.stack.push_back(&location.qualifiedName);
            state.error = std::move(externalError);
            if (state.error.code == ErrorCode::None) {
                state.error.code = ErrorCode::ExternalFunctionUnresolved;
                state.error.message = "external function '" + reference.name + "' failed";
            }
            state.error.stackTrace = materializeStack(state.stack);
            std::reverse(state.error.stackTrace.begin(), state.error.stackTrace.end());
            populateInterpreterSourceLocations(state);
            state.stack.pop_back();
            return false;
        }
        if (function.returnType == semantic::PrimitiveType::Void) {
            result = std::monostate{};
            return true;
        }
        if (!expectSignatureType(
                state,
                *externalResult,
                function.returnType,
                function.returnTypeId,
                "external return value")) {
            return false;
        }
        result = std::move(*externalResult);
        return true;
    }

    if (state.fastAccounting && execution->canUseFlatFastPath) {
#if defined(__GNUC__) || defined(__clang__)
        #define RS_FLAT_LIKELY(x) __builtin_expect(!!(x), 1)
        #define RS_FLAT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
        #define RS_FLAT_LIKELY(x) (x)
        #define RS_FLAT_UNLIKELY(x) (x)
#endif
        const auto previousFunctionId = state.currentFunctionId;
        state.currentFunctionId = function.symbolId;
        struct RestoreFastFunctionId {
            State& state;
            semantic::SymbolId previous;
            ~RestoreFastFunctionId() { state.currentFunctionId = previous; }
        } restoreFastFunctionId{state, previousFunctionId};

        state.stack.push_back(&location.qualifiedName);
        ++state.statistics.functionCalls;
        if (state.stack.size() > state.statistics.maximumCallDepth) {
            state.statistics.maximumCallDepth = state.stack.size();
        }

        const std::size_t frameOffset = state.valueTop;
        const std::size_t frameEnd = frameOffset + execution->totalFrameSlots;
        const std::size_t previousSize = state.values.size();
        if (RS_FLAT_UNLIKELY(frameEnd > previousSize)) {
            if (frameEnd > state.values.capacity()) {
                state.values.reserve(std::max<std::size_t>(frameEnd, state.values.capacity() * 2 + 256));
            }
            state.values.resize(frameEnd);
        }

        Value* __restrict frameBase = state.values.data() + frameOffset;
        const std::size_t argCount = activeCoroutineFrame
            ? activeCoroutineFrame->savedArguments.size()
            : arguments.size();
        std::uint32_t initialEntryOffset = execution->entryStreamOffset;
        std::uint64_t prologueSpanBonus = 0;
        if (activeCoroutineFrame != nullptr) {
            const std::size_t nArgs = std::min(argCount, function.parameterTypes.size());
            for (std::size_t i = 0; i < nArgs; ++i) {
                frameBase[execution->paramOffset + i] = activeCoroutineFrame->savedArguments[i];
            }
            const std::size_t nLocs = std::min(
                activeCoroutineFrame->savedLocals.size(), function.localTypes.size());
            for (std::size_t i = 0; i < nLocs; ++i) {
                frameBase[execution->localOffset + i] = activeCoroutineFrame->savedLocals[i];
            }
            const std::size_t nRegs = std::min(
                activeCoroutineFrame->savedRegisters.size(), function.registerTypes.size());
            for (std::size_t i = 0; i < nRegs; ++i) {
                frameBase[execution->registerOffset + i] = activeCoroutineFrame->savedRegisters[i];
            }
            const auto foundResumeBlk = execution->blockIndices.find(
                activeCoroutineFrame->resumeBlockId);
            if (foundResumeBlk == execution->blockIndices.end()) {
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram, "invalid coroutine resume block id");
            }
            initialEntryOffset = execution->blockStreamOffsets[foundResumeBlk->second];
        } else if (execution->hasFastParamToLocalPrologue &&
            state.limits.gcWorkBudget == 0 &&
            execution->entryStreamOffset == 0 &&
            argCount > 0 &&
            RS_FLAT_LIKELY(state.executed + 2u * argCount <= state.limits.instructionBudget)) {
            const std::size_t locOff = execution->localOffset;
            const std::size_t nLocals = function.localTypes.size();
            Value* __restrict localBase = frameBase + locOff;
            for (std::size_t i = 0; i < argCount; ++i) {
                localBase[i] = arguments[i];
            }
            if (nLocals > argCount) {
                std::memset(static_cast<void*>(localBase + argCount), 0, (nLocals - argCount) * sizeof(Value));
            }
            prologueSpanBonus = 2u * argCount;
            initialEntryOffset = static_cast<std::uint32_t>(argCount);
        } else {
            for (std::size_t i = 0; i < argCount; ++i) {
                frameBase[i] = arguments[i];
            }
            const std::size_t dirtyEnd = std::min(frameEnd, previousSize);
            if (frameOffset + argCount < dirtyEnd) {
                std::memset(static_cast<void*>(frameBase + argCount), 0,
                    (dirtyEnd - (frameOffset + argCount)) * sizeof(Value));
            }
        }
        state.valueTop = frameEnd;

        const auto routeFastException = [&](bytecode::BlockId protectedBlock) -> std::uint32_t {
            if (!state.pendingException ||
                !std::holds_alternative<ObjectRef>(*state.pendingException)) {
                return detail::InvalidStreamOffset;
            }
            const auto reference = std::get<ObjectRef>(*state.pendingException);
            const auto actual = state.heap->objectTypeId(reference);
            if (!actual) return detail::InvalidStreamOffset;
            for (const auto& handler : function.exceptionHandlers) {
                if (std::find(handler.protectedBlocks.begin(),
                              handler.protectedBlocks.end(), protectedBlock) ==
                    handler.protectedBlocks.end()) {
                    continue;
                }
                if (handler.catchTypeId != 0 &&
                    !runtimeAssignable(state, *actual, handler.catchTypeId)) {
                    continue;
                }
                if (handler.exceptionLocal >= function.localTypes.size()) {
                    return detail::InvalidStreamOffset;
                }
                frameBase[execution->localOffset + handler.exceptionLocal] = *state.pendingException;
                state.pendingException.reset();
                const auto foundBlk = execution->blockIndices.find(handler.handlerBlock);
                if (foundBlk == execution->blockIndices.end()) {
                    return detail::InvalidStreamOffset;
                }
                return execution->blockStreamOffsets[foundBlk->second];
            }
            return detail::InvalidStreamOffset;
        };

        std::uint64_t localExecuted = state.executed + prologueSpanBonus;
        std::uint64_t gcAccounted = localExecuted;
        const bool gcBudgetEnabled = (state.limits.gcWorkBudget != 0 && state.heap != nullptr);
        bool gcActive = gcBudgetEnabled &&
            (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
        struct FlatArrayCacheEntry {
            ObjectRef ref{};
            ArrayFastView view{};
            bool valid = false;
        };
        struct FlatObjectCacheEntry {
            ObjectRef ref{};
            ObjectFastView view{};
            semantic::SymbolId verifiedBaseTid = 0;
            bool valid = false;
        };
        std::array<FlatArrayCacheEntry, 4> arrayCache{};
        std::array<FlatObjectCacheEntry, 4> objectCache{};
        semantic::SymbolId cachedAssignableSubTid = 0;
        semantic::SymbolId cachedAssignableBaseTid = 0;

        const auto invalidateAllFlatCaches = [&]() noexcept {
            arrayCache[0].valid = false;
            arrayCache[1].valid = false;
            arrayCache[2].valid = false;
            arrayCache[3].valid = false;
            objectCache[0].valid = false;
            objectCache[1].valid = false;
            objectCache[2].valid = false;
            objectCache[3].valid = false;
        };

        const auto bindArrayCacheSlot = [&](const Value& arrVal, FlatArrayCacheEntry& ce) noexcept -> bool {
            if (RS_FLAT_UNLIKELY(arrVal.rawTag() != ValueTag::ObjectRef || state.heap == nullptr)) {
                return false;
            }
            const ObjectRef ref = arrVal.u_.objectRef;
            if (RS_FLAT_UNLIKELY(!state.heap->tryBindArrayFastView(ref, ce.view))) {
                return false;
            }
            ce.ref = ref;
            ce.valid = true;
            return true;
        };

        const auto bindObjectCacheEntry = [&](const ObjectRef& ref, FlatObjectCacheEntry& oce) noexcept -> bool {
            if (RS_FLAT_UNLIKELY(state.heap == nullptr || !state.heap->tryBindObjectFastView(ref, oce.view))) {
                return false;
            }
            oce.ref = ref;
            oce.verifiedBaseTid = 0;
            oce.valid = true;
            return true;
        };

        const auto flushFlatGc = [&]() {
            if (localExecuted > gcAccounted) {
                state.executed = localExecuted;
                stepGcIfNeeded(state, localExecuted - gcAccounted);
                gcAccounted = localExecuted;
                invalidateAllFlatCaches();
            }
            gcActive = gcBudgetEnabled &&
                (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
        };
        const auto syncAfterFlatSubcall = [&]() {
            localExecuted = state.executed;
            gcAccounted = localExecuted;
            invalidateAllFlatCaches();
            if (gcBudgetEnabled) {
                gcActive = (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
            }
        };
        const std::uint64_t budgetLimit = state.limits.instructionBudget;
        const detail::FlatInstruction* const streamBase = execution->flatStream.data();
        const detail::FlatColdMeta* const flatColdBase = execution->flatCold.data();
        const detail::FlatInstruction* ip = streamBase + initialEntryOffset;

#if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wpedantic"
        static constexpr const void* kFlatDispatchTable[] = {
            &&L_MoveSlot,
            &&L_ConstantInt,
            &&L_AddIntChecked,
            &&L_AddIntUnchecked,
            &&L_SubIntChecked,
            &&L_SubIntUnchecked,
            &&L_MulIntChecked,
            &&L_MulIntUnchecked,
            &&L_DivInt,
            &&L_RemInt,
            &&L_LessInt,
            &&L_LessOrEqualInt,
            &&L_GreaterInt,
            &&L_GreaterOrEqualInt,
            &&L_EqualInt,
            &&L_NotEqualInt,
            &&L_SuperMoveParamStoreLocal,
            &&L_SuperBinaryLocalIntConst,
            &&L_SuperBinaryLocalIntConstStore,
            &&L_SuperBinaryLocalIntConstStoreElided,
            &&L_SuperBinaryLocalIntLocal,
            &&L_SuperBinaryLocalIntLocalStore,
            &&L_SuperBinaryLocalIntLocalStoreElided,
            &&L_SuperBinaryLocalIntReg,
            &&L_SuperBinaryLocalIntRegStore,
            &&L_SuperBinaryLocalIntRegStoreElided,
            &&L_SuperBinaryLocalIntRegChainStore,
            &&L_SuperBinaryLocalIntRegChainStoreElided,
            &&L_SuperLessLocalRegBranch,
            &&L_SuperLessLocalConstBranch,
            &&L_CheckNotNullFast,
            &&L_LoadElementIntFast,
            &&L_StoreElementIntFast,
            &&L_CallDirect0,
            &&L_CallDirect1,
            &&L_CallDirect2,
            &&L_CallDirectN,
            &&L_CallPolledFast,
            &&L_ConstantDouble,
            &&L_AddDouble,
            &&L_SubDouble,
            &&L_MulDouble,
            &&L_DivDouble,
            &&L_LessDouble,
            &&L_LessOrEqualDouble,
            &&L_GreaterDouble,
            &&L_GreaterOrEqualDouble,
            &&L_LoadFieldFast,
            &&L_StoreFieldFast,
            &&L_LoadStructFieldFast,
            &&L_StoreStructFieldFast,
            &&L_ConstantBool,
            &&L_ConstantLong,
            &&L_ArrayLengthFast,
            &&L_ConvertIntToLongFast,
            &&L_ConvertIntToDoubleFast,
            &&L_ConvertLongToDoubleFast,
            &&L_SuperBinaryLocalDoubleConstStore,
            &&L_SuperBinaryLocalDoubleConstStoreElided,
            &&L_SuperBinaryLocalDoubleLocalStore,
            &&L_SuperBinaryLocalDoubleLocalStoreElided,
            &&L_NegateIntUnchecked,
            &&L_NegateDouble,
            &&L_LogicalNot,
            &&L_NewArrayFast,
            &&L_AddFloat,
            &&L_SubFloat,
            &&L_MulFloat,
            &&L_DivFloat,
            &&L_LessFloat,
            &&L_LessOrEqualFloat,
            &&L_GreaterFloat,
            &&L_GreaterOrEqualFloat,
            &&L_EqualFloat,
            &&L_NotEqualFloat,
            &&L_NegateFloat,
            &&L_ConvertIntToFloatFast,
            &&L_ConvertDoubleToFloatFast,
            &&L_ConvertFloatToIntFast,
            &&L_ConvertFloatToDoubleFast,
            &&L_ConvertLongToIntFast,
            &&L_SuperConstFloat,
            &&L_SuperConstFloatStoreLocal,
            &&L_SuperBinaryLocalFloatLocalStore,
            &&L_SuperBinaryLocalFloatLocalStoreElided,
            &&L_SuperBinaryLocalFloatReg,
            &&L_SuperBinaryLocalFloatRegElided,
            &&L_SuperBinaryLocalFloatRegStore,
            &&L_SuperBinaryLocalFloatRegStoreElided,
            &&L_SuperBinaryRegFloatStoreElided,
            &&L_NewStructFast,
            &&L_LoadFixedRawFast,
            &&L_StoreFixedRawFast,
            &&L_LoadVec3FieldFast,
            &&L_StoreVec3FieldFast,
            &&L_LoadQuatFieldFast,
            &&L_StoreQuatFieldFast,
            &&L_MathFixedConst,
            &&L_MathFixedFromRaw,
            &&L_MathFixedFromInt,
            &&L_MathFixedRatio,
            &&L_MathFixedNeg,
            &&L_MathFixedAdd,
            &&L_MathFixedSub,
            &&L_MathFixedMul,
            &&L_MathFixedMulInt,
            &&L_MathFixedDiv,
            &&L_MathFixedDivInt,
            &&L_MathFixedSin,
            &&L_MathFixedCos,
            &&L_MathFixedSqrt,
            &&L_MathSqrt64,
            &&L_MathVec3Const,
            &&L_MathVec3Raw,
            &&L_MathVec3Of,
            &&L_MathVec3Neg,
            &&L_MathVec3Add,
            &&L_MathVec3Sub,
            &&L_MathVec3MulFixed,
            &&L_MathVec3MulInt,
            &&L_MathVec3DivFixed,
            &&L_MathVec3DivInt,
            &&L_MathVec3Dot,
            &&L_MathVec3Cross,
            &&L_MathVec2Add,
            &&L_MathVec2Sub,
            &&L_MathVec2MulFixed,
            &&L_MathVec2Dot,
            &&L_MathQuatIdentity,
            &&L_MathQuatDot,
            &&L_MathQuatMul,
            &&L_MathQuatRotate,
            &&L_MathQuatRotateVec3,
            &&L_SuperQuatRotateBoxed,
            &&L_MathQuatNormalize,
            &&L_SuperQuatNormalizedBoxed,
            &&L_MathQuatFromEuler,
            &&L_MathQuatLerp,
            &&L_SuperLoadVec3RawFast,
            &&L_SuperLoadQuatRawFast,
            &&L_SuperFixedMulAdd,
            &&L_SuperVec3MulAdd,
            &&L_SuperVec3AddQuatRotate,
            &&L_SuperQuatMulNormalized,
            &&L_SuperQuatMulChain,
            &&L_SuperQuatMulRotateVec3,
            &&L_SuperMoveParamStoreLocalElided,
            &&L_SuperBinaryLocalIntConstStoreJump,
            &&L_SuperBinaryLocalIntLocalStoreJump,
            &&L_SuperBinaryLocalIntRegStoreJump,
            &&L_SuperCheckNotNullCallPolledFast,
            &&L_InlineCallEnter,
            &&L_InlineReturnVoid,
            &&L_InlineReturnVoidJump,
            &&L_InlineReturnScalar,
            &&L_InlineReturnScalarJump,
            &&L_LoadElementTypedFast,
            &&L_StoreElementPrimFast,
            &&L_StoreElementStructFast,
            &&L_StoreElementObjFast,
            &&L_SuperLessLocalArrayLengthBranch,
            &&L_NewObjectFast,
            &&L_LoadFieldExactFast,
            &&L_StoreFieldExactFast,
            &&L_IsTypeObjFast,
            &&L_AsTypeObjFast,
            &&L_NewDelegateFast,
            &&L_CombineDelegateFast,
            &&L_RemoveDelegateFast,
            &&L_InvokeDelegateFast,
            &&L_AsyncEnter,
            &&L_AwaitSuspend,
            &&L_AwaitResume,
            &&L_AsyncResolve,
            &&L_GenericBytecode,
            &&L_TermJumpNoArgs,
            &&L_TermBranchBoolNoArgs,
            &&L_TermBranchGeneric,
            &&L_TermReturnVoid,
            &&L_TermReturnScalarFast,
            &&L_TermReturnGeneric,
            &&L_TermThrow,
        };
        static_assert(
            sizeof(kFlatDispatchTable) / sizeof(kFlatDispatchTable[0]) ==
                static_cast<std::size_t>(detail::FlatOpcode::TermThrow) + 1u,
            "kFlatDispatchTable must match FlatOpcode enum order and size");
        #define RS_FLAT_DISPATCH_NEXT() goto *kFlatDispatchTable[static_cast<std::uint8_t>((++ip)->flatOpcode)]
        #define RS_FLAT_DISPATCH_AT_IP() goto *kFlatDispatchTable[static_cast<std::uint8_t>(ip->flatOpcode)]
        #define RS_FLAT_CASE(op) L_##op:
        RS_FLAT_DISPATCH_AT_IP();
#else
        #define RS_FLAT_DISPATCH_NEXT() ++ip; continue
        #define RS_FLAT_DISPATCH_AT_IP() continue
        #define RS_FLAT_CASE(op) case detail::FlatOpcode::op:
        for (;;) {
            switch (ip->flatOpcode) {
#endif

            RS_FLAT_CASE(MoveSlot) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot] = frameBase[ip->src0Slot];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConstantInt) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot].storeInt64(ip->integerImmediate);
                RS_FLAT_DISPATCH_NEXT();
            }

            #define RS_FLAT_SPAN_CHECK() \
                const std::uint64_t spanCost = ip->span; \
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) { \
                    if (spanCost > 1u) goto L_FlatSuperFallback; \
                    goto L_FlatBudgetExceeded; \
                }

            RS_FLAT_CASE(AddIntChecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                const std::int64_t res = a + b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                    res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AddIntUnchecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a + b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubIntChecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                const std::int64_t res = a - b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                    res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubIntUnchecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a - b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulIntChecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                const std::int64_t res = a * b;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                    res > std::numeric_limits<std::int32_t>::max())) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulIntUnchecked) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(wrapInt32(a * b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(DivInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                if (RS_FLAT_UNLIKELY(b == 0)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::DivisionByZero, "division by zero");
                    goto L_FlatError;
                }
                if (RS_FLAT_UNLIKELY(a == std::numeric_limits<std::int32_t>::min() && b == -1)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::IntegerOverflow, "integer division overflow");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(a / b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(RemInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                if (RS_FLAT_UNLIKELY(b == 0)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::DivisionByZero, "remainder by zero");
                    goto L_FlatError;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeInt64(
                    (a == std::numeric_limits<std::int32_t>::min() && b == -1)
                        ? std::int64_t{0}
                        : (a % b));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a < b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessOrEqualInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a <= b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a > b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterOrEqualInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a >= b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(EqualInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a == b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NotEqualInt) {
                RS_FLAT_SPAN_CHECK();
                std::int64_t a = 0, b = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(a) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(b))) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    ++localExecuted;
                    goto L_FlatTypeMismatch;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(a != b);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperMoveParamStoreLocal) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                const auto& val = frameBase[ip->src0Slot];
                frameBase[ip->dstSlot] = val;
                frameBase[ip->storeSlot] = val;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntConst) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) goto L_FlatTypeMismatch;
                const std::int64_t right = ip->integerImmediate;
                frameBase[ip->dstSlot].storeInt64(left);
                frameBase[ip->aux0Slot].storeInt64(right);
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->aux1Slot]))) {
                    goto L_FlatError;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntConstStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) goto L_FlatTypeMismatch;
                const std::int64_t right = ip->integerImmediate;
                frameBase[ip->dstSlot].storeInt64(left);
                frameBase[ip->aux0Slot].storeInt64(right);
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(
                        state, ip->binaryOpcode1, ip->checked1(), left, right,
                        frameBase[ip->aux1Slot], &frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntConstStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) goto L_FlatTypeMismatch;
                const std::int64_t right = ip->integerImmediate;
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntLocal) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                frameBase[ip->aux0Slot].storeInt64(right);
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->aux1Slot]))) {
                    goto L_FlatError;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntLocalStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                frameBase[ip->aux0Slot].storeInt64(right);
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(
                        state, ip->binaryOpcode1, ip->checked1(), left, right,
                        frameBase[ip->aux1Slot], &frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntLocalStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                localExecuted += 3;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntReg) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                localExecuted += 2;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->aux1Slot]))) {
                    goto L_FlatError;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                localExecuted += 2;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(
                        state, ip->binaryOpcode1, ip->checked1(), left, right,
                        frameBase[ip->aux1Slot], &frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                localExecuted += 2;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegChainStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right1 = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right1))) goto L_FlatTypeMismatch;
                frameBase[ip->dstSlot].storeInt64(left);
                localExecuted += 2;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right1, frameBase[ip->aux0Slot]))) {
                    goto L_FlatError;
                }
                const auto midInt = frameBase[ip->aux0Slot].asInt64Unchecked();
                std::int64_t right2 = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src2Slot].tryGetInt64(right2))) goto L_FlatTypeMismatch;
                ++localExecuted;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(
                        state, ip->binaryOpcode2, ip->checked2(), midInt, right2,
                        frameBase[ip->aux1Slot], &frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegChainStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right1 = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right1))) goto L_FlatTypeMismatch;
                localExecuted += 2;
                state.executed = localExecuted;
                Value midOutput;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode1, ip->checked1(), left, right1, midOutput))) {
                    goto L_FlatError;
                }
                const auto midInt = midOutput.asInt64Unchecked();
                std::int64_t right2 = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src2Slot].tryGetInt64(right2))) goto L_FlatTypeMismatch;
                ++localExecuted;
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!evaluateQuickIntInPlace(state, ip->binaryOpcode2, ip->checked2(), midInt, right2, frameBase[ip->storeSlot]))) {
                    goto L_FlatError;
                }
                ++localExecuted;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperLessLocalRegBranch) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatTypeMismatch;
                localExecuted += 3;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                bool cond = false;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::LessInt: cond = (left < right); break;
                case detail::QuickOpcode::LessOrEqualInt: cond = (left <= right); break;
                case detail::QuickOpcode::GreaterInt: cond = (left > right); break;
                case detail::QuickOpcode::GreaterOrEqualInt: cond = (left >= right); break;
                case detail::QuickOpcode::EqualInt: cond = (left == right); break;
                case detail::QuickOpcode::NotEqualInt: cond = (left != right); break;
                default: goto L_FlatSuperFallback;
                }
                ++state.statistics.branchesTaken;
                ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(SuperLessLocalConstBranch) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) goto L_FlatTypeMismatch;
                localExecuted += 4;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const std::int64_t right = ip->integerImmediate;
                bool cond = false;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::LessInt: cond = (left < right); break;
                case detail::QuickOpcode::LessOrEqualInt: cond = (left <= right); break;
                case detail::QuickOpcode::GreaterInt: cond = (left > right); break;
                case detail::QuickOpcode::GreaterOrEqualInt: cond = (left >= right); break;
                case detail::QuickOpcode::EqualInt: cond = (left == right); break;
                case detail::QuickOpcode::NotEqualInt: cond = (left != right); break;
                default: goto L_FlatSuperFallback;
                }
                ++state.statistics.branchesTaken;
                ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(CheckNotNullFast) {
                const Value& val = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(val.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = val.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        RS_FLAT_SPAN_CHECK();
                        localExecuted += spanCost;
                        frameBase[ip->dstSlot] = val;
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(LoadElementIntFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(ce.view.elementType != semantic::PrimitiveType::Int)) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_SPAN_CHECK();
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot] = ce.view.data[uIdx];
                    RS_FLAT_DISPATCH_NEXT();
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = ce.view.data[static_cast<std::size_t>(idx)];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreElementIntFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const Value& storeVal = frameBase[ip->src2Slot];
                if (RS_FLAT_UNLIKELY(ce.view.elementType != semantic::PrimitiveType::Int ||
                                     storeVal.rawTag() != ValueTag::Int64)) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_SPAN_CHECK();
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    localExecuted += spanCost;
                    ce.view.data[uIdx] = storeVal;
                    RS_FLAT_DISPATCH_NEXT();
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                ce.view.data[static_cast<std::size_t>(idx)] = storeVal;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallDirect0) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                const ValueWindow regWin{&state.values, frameOffset + execution->registerOffset, function.registerTypes.size()};
                const bool ok = executeFunction(
                    state, *cold.directTarget, ArgumentSource(regWin),
                    callResult, cold.directExecution, true);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = std::move(callResult);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallDirect1) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                const ValueWindow singleWin{&state.values, frameOffset + ip->src0Slot, 1};
                const bool ok = executeFunction(
                    state, *cold.directTarget,
                    ArgumentSource(singleWin, static_cast<bytecode::Register>(0)),
                    callResult, cold.directExecution, true);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = std::move(callResult);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallDirect2) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                const ValueWindow baseWin{&state.values, frameOffset, execution->totalFrameSlots};
                const bool ok = executeFunction(
                    state, *cold.directTarget,
                    ArgumentSource(
                        baseWin,
                        static_cast<bytecode::Register>(ip->src0Slot),
                        static_cast<bytecode::Register>(ip->src1Slot)),
                    callResult, cold.directExecution, true);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = std::move(callResult);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallDirectN) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                const ValueWindow regWin{&state.values, frameOffset + execution->registerOffset, function.registerTypes.size()};
                const bool ok = executeFunction(
                    state, *cold.directTarget,
                    ArgumentSource(regWin, cold.bytecode->operands),
                    callResult, cold.directExecution, true);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = std::move(callResult);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(CallPolledFast) {
            L_CallPolledFastBody:
                const std::uint64_t baseCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || baseCost > budgetLimit - localExecuted)) {
                    if (baseCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const semantic::SymbolId actualTid = oce.view.typeId;
                        const std::uint32_t siteId = ip->auxImm32;
                        const PolymorphicPicWay* hit = nullptr;
                        if (RS_FLAT_LIKELY(state.polyCallCache != nullptr &&
                                           siteId < state.polyCallCache->size())) {
                            auto& pic = (*state.polyCallCache)[siteId];
                            if (pic.ways[0].receiverTypeId == actualTid) hit = &pic.ways[0];
                            else if (pic.ways[1].receiverTypeId == actualTid) hit = &pic.ways[1];
                            else if (pic.ways[2].receiverTypeId == actualTid) hit = &pic.ways[2];
                            else if (pic.ways[3].receiverTypeId == actualTid) hit = &pic.ways[3];
                            else if (state.mirror && siteId < state.mirror->polyCallSites.size()) {
                                if (const auto* pre = resolvePrelinkedPolymorphicTarget(
                                        *state.mirror, actualTid, state.mirror->polyCallSites[siteId])) {
                                    auto& slotWay = pic.ways[pic.nextReplace & 3u];
                                    pic.nextReplace = static_cast<std::uint8_t>((pic.nextReplace + 1u) & 3u);
                                    slotWay = {actualTid, pre->target, pre->execution};
                                    hit = &slotWay;
                                }
                            }
                        }
                        if (RS_FLAT_LIKELY(hit != nullptr)) {
                            const auto* calleeExec = hit->execution;
                            const auto tKind = calleeExec->trivialKind;
                            if (tKind != detail::TrivialMethodKind::None &&
                                RS_FLAT_LIKELY(state.stack.size() < state.limits.recursionLimit)) {
                                const std::uint64_t totalCost = baseCost + calleeExec->trivialInstCost;
                                if (RS_FLAT_LIKELY(totalCost <= budgetLimit - localExecuted)) {
                                    bool trivialHandled = false;
                                    Value trivialOut{};
                                    switch (tKind) {
                                    case detail::TrivialMethodKind::ReturnVoid:
                                        trivialHandled = true;
                                        break;
                                    case detail::TrivialMethodKind::ReturnConst:
                                        trivialOut = calleeExec->trivialConstValue;
                                        trivialHandled = true;
                                        break;
                                    case detail::TrivialMethodKind::ReturnParam: {
                                        const std::uint16_t pIdx = calleeExec->trivialParamIndex;
                                        if (pIdx == 0) {
                                            trivialOut = recvVal;
                                            trivialHandled = true;
                                        } else if (pIdx == 1 && ip->inlineArgCount >= 2u) {
                                            trivialOut = frameBase[ip->src1Slot];
                                            trivialHandled = true;
                                        } else if (pIdx == 2 && ip->inlineArgCount >= 3u) {
                                            trivialOut = frameBase[ip->src2Slot];
                                            trivialHandled = true;
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::ReturnThisFieldExact: {
                                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                                        if (RS_FLAT_LIKELY(f0 < oce.view.fieldCount)) {
                                            const Value& fv = oce.view.fields[f0];
                                            if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(fv.rawTag()) ==
                                                               calleeExec->trivialFieldTag0)) {
                                                trivialOut = fv;
                                                trivialHandled = true;
                                            }
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::StoreThisFieldExact: {
                                        const std::uint16_t pIdx = calleeExec->trivialParamIndex;
                                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                                        if (pIdx == 1 && ip->inlineArgCount >= 2u &&
                                            RS_FLAT_LIKELY(f0 < oce.view.fieldCount)) {
                                            const Value& rhs = frameBase[ip->src1Slot];
                                            if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(rhs.rawTag()) ==
                                                               calleeExec->trivialFieldTag0)) {
                                                oce.view.fields[f0] = rhs;
                                                trivialHandled = true;
                                            }
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::ReturnThisFieldAddField:
                                    case detail::TrivialMethodKind::ReturnThisFieldSubField: {
                                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                                        const std::uint16_t f1 = calleeExec->trivialFieldIndex1;
                                        std::int64_t a = 0, b = 0;
                                        if (RS_FLAT_LIKELY(f0 < oce.view.fieldCount &&
                                                           f1 < oce.view.fieldCount &&
                                                           oce.view.fields[f0].tryGetInt64(a) &&
                                                           oce.view.fields[f1].tryGetInt64(b))) {
                                            const std::int64_t r =
                                                (tKind == detail::TrivialMethodKind::ReturnThisFieldAddField)
                                                    ? (a + b)
                                                    : (a - b);
                                            if (!calleeExec->trivialCheckedArith) {
                                                trivialOut.storeInt64(wrapInt32(r));
                                                trivialHandled = true;
                                            } else if (RS_FLAT_LIKELY(isValidInt32Payload(r))) {
                                                trivialOut.storeInt64(r);
                                                trivialHandled = true;
                                            }
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::ReturnThisFieldAddConst:
                                    case detail::TrivialMethodKind::ReturnThisFieldSubConst:
                                    case detail::TrivialMethodKind::ReturnThisFieldMulConst: {
                                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                                        std::int64_t a = 0;
                                        if (RS_FLAT_LIKELY(f0 < oce.view.fieldCount &&
                                                           oce.view.fields[f0].tryGetInt64(a))) {
                                            const std::int64_t c =
                                                calleeExec->trivialConstValue.asInt64Unchecked();
                                            std::int64_t r = 0;
                                            bool ovf = false;
                                            if (tKind == detail::TrivialMethodKind::ReturnThisFieldAddConst) {
                                                r = a + c;
                                            } else if (tKind == detail::TrivialMethodKind::ReturnThisFieldSubConst) {
                                                r = a - c;
                                            } else {
#if defined(__GNUC__) || defined(__clang__)
                                                ovf = __builtin_mul_overflow(a, c, &r);
#else
                                                if (RS_FLAT_LIKELY(isValidInt32Payload(a) && isValidInt32Payload(c))) {
                                                    r = a * c;
                                                } else {
                                                    ovf = true;
                                                }
#endif
                                            }
                                            if (!ovf) {
                                                if (!calleeExec->trivialCheckedArith) {
                                                    trivialOut.storeInt64(wrapInt32(r));
                                                    trivialHandled = true;
                                                } else if (RS_FLAT_LIKELY(isValidInt32Payload(r))) {
                                                    trivialOut.storeInt64(r);
                                                    trivialHandled = true;
                                                }
                                            }
                                        }
                                        break;
                                    }
                                    case detail::TrivialMethodKind::None:
                                        break;
                                    }
                                    if (RS_FLAT_LIKELY(trivialHandled)) {
                                        localExecuted += totalCost;
                                        ++state.statistics.functionCalls;
                                        if (state.stack.size() + 1 > state.statistics.maximumCallDepth) {
                                            state.statistics.maximumCallDepth = state.stack.size() + 1;
                                        }
                                        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                                        if (ip->dstSlot != detail::InvalidCompactReg) {
                                            frameBase[ip->dstSlot] = trivialOut;
                                        }
                                        RS_FLAT_DISPATCH_NEXT();
                                    }
                                }
                            }
                            localExecuted += baseCost;
                            if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                            state.executed = localExecuted;
                            Value callResult;
                            const ValueWindow baseWin{&state.values, frameOffset, execution->totalFrameSlots};
                            bool ok = false;
                            if (ip->inlineArgCount == 1u) {
                                ok = executeFunction(
                                    state, *hit->target,
                                    ArgumentSource(baseWin, static_cast<bytecode::Register>(ip->src0Slot)),
                                    callResult, calleeExec, true);
                            } else if (ip->inlineArgCount == 2u) {
                                ok = executeFunction(
                                    state, *hit->target,
                                    ArgumentSource(
                                        baseWin,
                                        static_cast<bytecode::Register>(ip->src0Slot),
                                        static_cast<bytecode::Register>(ip->src1Slot)),
                                    callResult, calleeExec, true);
                            } else if (ip->inlineArgCount == 3u) {
                                ok = executeFunction(
                                    state, *hit->target,
                                    ArgumentSource(
                                        baseWin,
                                        static_cast<bytecode::Register>(ip->src0Slot),
                                        static_cast<bytecode::Register>(ip->src1Slot),
                                        static_cast<bytecode::Register>(ip->src2Slot)),
                                    callResult, calleeExec, true);
                            } else {
                                const auto& cold = flatColdBase[ip - streamBase];
                                const ValueWindow regWin{
                                    &state.values,
                                    frameOffset + execution->registerOffset,
                                    function.registerTypes.size()};
                                ok = executeFunction(
                                    state, *hit->target,
                                    ArgumentSource(regWin, cold.bytecode->operands),
                                    callResult, calleeExec, true);
                            }
                            frameBase = state.values.data() + frameOffset;
                            if (RS_FLAT_LIKELY(calleeExec->isSmallLeafInlineCandidate && !gcBudgetEnabled)) {
                                localExecuted = state.executed;
                                gcAccounted = localExecuted;
                            } else {
                                syncAfterFlatSubcall();
                            }
                            if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                            if (ip->dstSlot != detail::InvalidCompactReg) {
                                frameBase[ip->dstSlot] = std::move(callResult);
                            }
                            RS_FLAT_DISPATCH_NEXT();
                        }
                    }
                }
                if (baseCost > 1u) goto L_FlatSuperFallback;
                ++localExecuted;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                Value callResult;
                const ValueWindow regWin{&state.values, frameOffset + execution->registerOffset, function.registerTypes.size()};
                const bool ok = executeCall(state, *location.module, *cold.bytecode, regWin, callResult);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = std::move(callResult);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConstantDouble) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                double dVal = 0.0;
                std::memcpy(&dVal, &ip->integerImmediate, sizeof(double));
                frameBase[ip->dstSlot].storeDouble(dVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AddDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(aVal + bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(aVal - bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(aVal * bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(DivDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeDouble(aVal / bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal < bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessOrEqualDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal <= bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal > bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterOrEqualDouble) {
                double aVal = 0.0, bVal = 0.0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetDouble(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetDouble(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal >= bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadFieldFast) {
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool ownerOk = (oce.view.typeId == expectedOwnerTid) ||
                                       (oce.verifiedBaseTid == expectedOwnerTid);
                        if (RS_FLAT_UNLIKELY(!ownerOk)) {
                            if (runtimeAssignable(state, oce.view.typeId, expectedOwnerTid)) {
                                oce.verifiedBaseTid = expectedOwnerTid;
                                ownerOk = true;
                            }
                        }
                        const std::uint16_t fIdx = ip->aux0Slot;
                        if (RS_FLAT_LIKELY(ownerOk && fIdx < oce.view.fieldCount)) {
                            const Value& val = oce.view.fields[fIdx];
                            const auto fk = static_cast<detail::FastFieldKind>(
                                static_cast<std::uint8_t>(ip->binaryOpcode2));
                            if (RS_FLAT_LIKELY(fk == detail::FastFieldKind::ExactValueTag)) {
                                if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(val.rawTag()) ==
                                                   static_cast<std::uint8_t>(ip->binaryOpcode1))) {
                                    RS_FLAT_SPAN_CHECK();
                                    localExecuted += spanCost;
                                    frameBase[ip->dstSlot] = val;
                                    RS_FLAT_DISPATCH_NEXT();
                                }
                            } else {
                                const ValueTag vt = val.rawTag();
                                bool fieldOk = false;
                                switch (fk) {
                                case detail::FastFieldKind::Quat:
                                    fieldOk = (vt == ValueTag::QuatInline || vt == ValueTag::QuatArena);
                                    break;
                                case detail::FastFieldKind::String:
                                    fieldOk = (vt == ValueTag::NullString || vt == ValueTag::String ||
                                               (vt == ValueTag::ObjectRef &&
                                                val.u_.objectRef.kind == ObjectKind::String &&
                                                state.heap->isAlive(val.u_.objectRef)));
                                    break;
                                case detail::FastFieldKind::Object:
                                    if (vt == ValueTag::NullObject) {
                                        fieldOk = true;
                                    } else if (vt == ValueTag::ObjectRef) {
                                        const auto expTid = detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId;
                                        const auto actTid = state.heap->objectTypeIdFast(val.u_.objectRef);
                                        if (actTid != 0) {
                                            if (actTid == expTid || expTid == 0 ||
                                                (actTid == cachedAssignableSubTid && expTid == cachedAssignableBaseTid)) {
                                                fieldOk = true;
                                            } else if (runtimeAssignable(state, actTid, expTid)) {
                                                cachedAssignableSubTid = actTid;
                                                cachedAssignableBaseTid = expTid;
                                                fieldOk = true;
                                            }
                                        }
                                    }
                                    break;
                                case detail::FastFieldKind::Array:
                                    if (vt == ValueTag::NullArray) {
                                        fieldOk = true;
                                    } else if (vt == ValueTag::ObjectRef) {
                                        const auto expTid = detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId;
                                        const auto actTid = state.heap->arrayTypeId(val.u_.objectRef);
                                        fieldOk = (actTid.has_value() && (*actTid == expTid || expTid == 0));
                                    }
                                    break;
                                case detail::FastFieldKind::UserStruct:
                                    fieldOk = (vt == ValueTag::Struct &&
                                               val.u_.structVal.valid() &&
                                               val.u_.structVal.typeId == detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId);
                                    break;
                                case detail::FastFieldKind::Enum: {
                                    EnumValue ev{};
                                    fieldOk = (val.tryGetEnum(ev) &&
                                               ev.typeId == detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId);
                                    break;
                                }
                                default:
                                    break;
                                }
                                if (RS_FLAT_LIKELY(fieldOk)) {
                                    RS_FLAT_SPAN_CHECK();
                                    localExecuted += spanCost;
                                    frameBase[ip->dstSlot] = val;
                                    RS_FLAT_DISPATCH_NEXT();
                                }
                            }
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(StoreFieldFast) {
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool ownerOk = (oce.view.typeId == expectedOwnerTid) ||
                                       (oce.verifiedBaseTid == expectedOwnerTid);
                        if (RS_FLAT_UNLIKELY(!ownerOk)) {
                            if (runtimeAssignable(state, oce.view.typeId, expectedOwnerTid)) {
                                oce.verifiedBaseTid = expectedOwnerTid;
                                ownerOk = true;
                            }
                        }
                        const std::uint16_t fIdx = ip->aux0Slot;
                        if (RS_FLAT_LIKELY(ownerOk && fIdx < oce.view.fieldCount)) {
                            const Value& rhs = frameBase[ip->src1Slot];
                            const auto fk = static_cast<detail::FastFieldKind>(
                                static_cast<std::uint8_t>(ip->binaryOpcode2));
                            if (RS_FLAT_LIKELY(fk == detail::FastFieldKind::ExactValueTag)) {
                                if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(rhs.rawTag()) ==
                                                   static_cast<std::uint8_t>(ip->binaryOpcode1))) {
                                    RS_FLAT_SPAN_CHECK();
                                    localExecuted += spanCost;
                                    oce.view.fields[fIdx] = rhs;
                                    RS_FLAT_DISPATCH_NEXT();
                                }
                            } else {
                                const ValueTag vt = rhs.rawTag();
                                bool rhsOk = false;
                                bool needsPin = false;
                                bool needsBarrier = false;
                                switch (fk) {
                                case detail::FastFieldKind::Quat:
                                    if (vt == ValueTag::QuatInline) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::QuatArena) {
                                        rhsOk = true;
                                        needsPin = true;
                                    }
                                    break;
                                case detail::FastFieldKind::String:
                                    if (vt == ValueTag::NullString) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::String) {
                                        rhsOk = true;
                                        needsPin = true;
                                    } else if (vt == ValueTag::ObjectRef &&
                                               rhs.u_.objectRef.kind == ObjectKind::String &&
                                               state.heap->isAlive(rhs.u_.objectRef)) {
                                        rhsOk = true;
                                        needsBarrier = true;
                                    }
                                    break;
                                case detail::FastFieldKind::Object:
                                    if (vt == ValueTag::NullObject) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::ObjectRef) {
                                        const auto expTid = detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId;
                                        const auto actTid = state.heap->objectTypeIdFast(rhs.u_.objectRef);
                                        if (actTid != 0) {
                                            if (actTid == expTid || expTid == 0 ||
                                                (actTid == cachedAssignableSubTid && expTid == cachedAssignableBaseTid)) {
                                                rhsOk = true;
                                                needsBarrier = true;
                                            } else if (runtimeAssignable(state, actTid, expTid)) {
                                                cachedAssignableSubTid = actTid;
                                                cachedAssignableBaseTid = expTid;
                                                rhsOk = true;
                                                needsBarrier = true;
                                            }
                                        }
                                    }
                                    break;
                                case detail::FastFieldKind::Array:
                                    if (vt == ValueTag::NullArray) {
                                        rhsOk = true;
                                    } else if (vt == ValueTag::ObjectRef) {
                                        const auto expTid = detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId;
                                        const auto actTid = state.heap->arrayTypeId(rhs.u_.objectRef);
                                        if (actTid.has_value() && (*actTid == expTid || expTid == 0)) {
                                            rhsOk = true;
                                            needsBarrier = true;
                                        }
                                    }
                                    break;
                                case detail::FastFieldKind::UserStruct:
                                    if (vt == ValueTag::Struct &&
                                        rhs.u_.structVal.valid() &&
                                        rhs.u_.structVal.typeId == detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId) {
                                        rhsOk = true;
                                        needsPin = true;
                                        needsBarrier = !rhs.u_.structVal.isInline && rhs.u_.structVal.storage != nullptr;
                                    }
                                    break;
                                case detail::FastFieldKind::Enum: {
                                    EnumValue ev{};
                                    if (rhs.tryGetEnum(ev) &&
                                        ev.typeId == detail::executionMirror(*state.program).fieldMetas[ip->auxImm32].fieldTypeId) {
                                        rhsOk = true;
                                        needsPin = (vt == ValueTag::EnumArena);
                                    }
                                    break;
                                }
                                default:
                                    break;
                                }
                                if (RS_FLAT_LIKELY(rhsOk)) {
                                    RS_FLAT_SPAN_CHECK();
                                    if (needsPin) pinValueInPools(rhs);
                                    if (needsBarrier &&
                                        (oce.view.needsRememberedBarrier || gcActive || state.heap->phase() != GcPhase::Idle)) {
                                        state.heap->recordObjectWriteBarrier(oce.ref, rhs);
                                        oce.view.needsRememberedBarrier = false;
                                    }
                                    localExecuted += spanCost;
                                    oce.view.fields[fIdx] = rhs;
                                    RS_FLAT_DISPATCH_NEXT();
                                }
                            }
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(LoadStructFieldFast) {
                const auto* type = typeDescriptor(*location.module, ip->auxImm32);
                const auto& recv = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(type != nullptr && structValueTypeId(recv) == type->id)) {
                    const auto expectedFieldType = type->fields[ip->aux0Slot].type;
                    const auto expectedFieldTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                    if (auto inlinedField = loadInlineStructField(recv, ip->aux0Slot);
                        RS_FLAT_LIKELY(inlinedField.has_value() &&
                        valueType(*inlinedField) == expectedFieldType &&
                        (expectedFieldTid == 0 || structValueTypeId(*inlinedField) == expectedFieldTid))) {
                        if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                        frameBase[ip->dstSlot] = std::move(*inlinedField);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatGenericFallback;
            }

            RS_FLAT_CASE(StoreStructFieldFast) {
                const auto* type = typeDescriptor(*location.module, ip->auxImm32);
                const auto& recv = frameBase[ip->src0Slot];
                const auto& rhs = frameBase[ip->src1Slot];
                const auto expectedFieldTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (RS_FLAT_LIKELY(type != nullptr && structValueTypeId(recv) == type->id &&
                    valueType(rhs) == type->fields[ip->aux0Slot].type &&
                    (expectedFieldTid == 0 || structValueTypeId(rhs) == expectedFieldTid))) {
                    if (auto updated = storeInlineStructField(recv, ip->aux0Slot, rhs);
                        RS_FLAT_LIKELY(updated.has_value())) {
                        if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                        frameBase[ip->dstSlot] = std::move(*updated);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatGenericFallback;
            }

            RS_FLAT_CASE(ConstantBool) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot] = (ip->integerImmediate != 0);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConstantLong) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot].storeLong(ip->integerImmediate);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ArrayLengthFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY((ce.valid &&
                                    arrVal.rawTag() == ValueTag::ObjectRef &&
                                    arrVal.u_.objectRef == ce.ref) ||
                                   bindArrayCacheSlot(arrVal, ce))) {
                    RS_FLAT_SPAN_CHECK();
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(ce.view.length));
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (const auto* ref = std::get_if<ObjectRef>(&arrVal); RS_FLAT_LIKELY(ref != nullptr && state.heap)) {
                    if (const auto len = state.heap->arrayLength(*ref);
                        RS_FLAT_LIKELY(len.has_value() &&
                        *len <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))) {
                        RS_FLAT_SPAN_CHECK();
                        localExecuted += spanCost;
                        frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(*len));
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(ConvertIntToLongFast) {
                const auto* vPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeLong(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertIntToDoubleFast) {
                const auto* vPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = static_cast<double>(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertLongToDoubleFast) {
                const auto* vPtr = std::get_if<LongValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = static_cast<double>(vPtr->value);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalDoubleConstStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                const auto* leftPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!leftPtr)) goto L_FlatSuperFallback;
                const double left = *leftPtr;
                double right = 0.0;
                std::memcpy(&right, &ip->integerImmediate, sizeof(double));
                double out = 0.0;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddDouble: out = left + right; break;
                case detail::QuickOpcode::SubtractDouble: out = left - right; break;
                case detail::QuickOpcode::MultiplyDouble: out = left * right; break;
                case detail::QuickOpcode::DivideDouble: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->dstSlot].storeDouble(left);
                frameBase[ip->aux0Slot].storeDouble(right);
                frameBase[ip->aux1Slot].storeDouble(out);
                frameBase[ip->storeSlot].storeDouble(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalDoubleConstStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                const auto* leftPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!leftPtr)) goto L_FlatSuperFallback;
                const double left = *leftPtr;
                double right = 0.0;
                std::memcpy(&right, &ip->integerImmediate, sizeof(double));
                double out = 0.0;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddDouble: out = left + right; break;
                case detail::QuickOpcode::SubtractDouble: out = left - right; break;
                case detail::QuickOpcode::MultiplyDouble: out = left * right; break;
                case detail::QuickOpcode::DivideDouble: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->storeSlot].storeDouble(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalDoubleLocalStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                const auto* leftPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                const auto* rightPtr = std::get_if<double>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!leftPtr || !rightPtr)) goto L_FlatSuperFallback;
                const double left = *leftPtr;
                const double right = *rightPtr;
                double out = 0.0;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddDouble: out = left + right; break;
                case detail::QuickOpcode::SubtractDouble: out = left - right; break;
                case detail::QuickOpcode::MultiplyDouble: out = left * right; break;
                case detail::QuickOpcode::DivideDouble: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->dstSlot].storeDouble(left);
                frameBase[ip->aux0Slot].storeDouble(right);
                frameBase[ip->aux1Slot].storeDouble(out);
                frameBase[ip->storeSlot].storeDouble(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalDoubleLocalStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                const auto* leftPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                const auto* rightPtr = std::get_if<double>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!leftPtr || !rightPtr)) goto L_FlatSuperFallback;
                const double left = *leftPtr;
                const double right = *rightPtr;
                double out = 0.0;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddDouble: out = left + right; break;
                case detail::QuickOpcode::SubtractDouble: out = left - right; break;
                case detail::QuickOpcode::MultiplyDouble: out = left * right; break;
                case detail::QuickOpcode::DivideDouble: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->storeSlot].storeDouble(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NegateIntUnchecked) {
                const auto* vPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = (*vPtr == std::numeric_limits<std::int32_t>::min())
                    ? *vPtr
                    : -(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NegateDouble) {
                const auto* vPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = -(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LogicalNot) {
                const auto* vPtr = std::get_if<bool>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = !(*vPtr);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NewArrayFast) {
                std::int64_t lenVal = 0;
                if (RS_FLAT_LIKELY(
                        frameBase[ip->src0Slot].tryGetInt64(lenVal) &&
                        lenVal >= 0 &&
                        static_cast<std::uint64_t>(lenVal) <=
                            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) &&
                        state.heap != nullptr)) {
                    RS_FLAT_SPAN_CHECK();
                    localExecuted += spanCost;
                    if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                    else gcAccounted = localExecuted;

                    const auto elemType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1));
                    const auto arrayTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                    Value initVal;
                    semantic::SymbolId elemTid = 0;
                    if (ip->aux0Slot == 1) {
                        switch (elemType) {
                        case semantic::PrimitiveType::Int: initVal.storeInt64(0); break;
                        case semantic::PrimitiveType::Float: initVal.storeFloat(0.0f); break;
                        case semantic::PrimitiveType::Double: initVal.storeDouble(0.0); break;
                        case semantic::PrimitiveType::Long: initVal.storeLong(0); break;
                        case semantic::PrimitiveType::Bool: initVal.storeBool(false); break;
                        case semantic::PrimitiveType::Object:
                            initVal = NullObject{};
                            elemTid = flatColdBase[ip - streamBase].bytecode->elementTypeId;
                            break;
                        case semantic::PrimitiveType::String: initVal = NullString{}; break;
                        default:
                            initVal = defaultValue(*location.module, elemType, 0);
                            break;
                        }
                    } else if (ip->aux0Slot >= 2 && ip->aux0Slot <= 6) {
                        elemTid = flatColdBase[ip - streamBase].bytecode->elementTypeId;
                        switch (ip->aux0Slot) {
                        case 2: initVal = FixedValue{}; break;
                        case 3: initVal = Fixed16Value{}; break;
                        case 4: initVal = Vec2Value{}; break;
                        case 5: initVal = Vec3Value{}; break;
                        default: initVal = QuatValue{}; break;
                        }
                    } else {
                        const auto& inst = *flatColdBase[ip - streamBase].bytecode;
                        elemTid = inst.elementTypeId;
                        initVal = defaultValue(*location.module, elemType, elemTid);
                    }

                    bool slotsMoved = false;
                    RuntimeError allocErr;
                    const auto ref = state.heap->allocateTypedArray(
                        arrayTid,
                        elemType,
                        elemTid,
                        static_cast<std::size_t>(lenVal),
                        std::move(initVal),
                        &allocErr,
                        &slotsMoved);
                    if (RS_FLAT_LIKELY(ref.has_value())) {
                        if (RS_FLAT_UNLIKELY(slotsMoved)) {
                            invalidateAllFlatCaches();
                        }
                        if (gcBudgetEnabled) {
                            gcActive = (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
                        }
                        frameBase[ip->dstSlot] = *ref;
                        auto& ce = arrayCache[ip->dstSlot & 3u];
                        if (state.heap->tryBindArrayFastView(*ref, ce.view)) {
                            ce.ref = *ref;
                            ce.valid = true;
                        }
                        RS_FLAT_DISPATCH_NEXT();
                    }
                    if (spanCost > 1u) {
                        localExecuted -= spanCost;
                        goto L_FlatSuperFallback;
                    }
                    state.executed = localExecuted;
                    fail(state,
                        allocErr.code == ErrorCode::None ? ErrorCode::OutOfMemory : allocErr.code,
                        allocErr.message.empty() ? "managed array allocation failed" : allocErr.message);
                    goto L_FlatError;
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(AddFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(aVal + bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SubFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(aVal - bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MulFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(aVal * bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(DivFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(aVal / bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal < bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LessOrEqualFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal <= bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal > bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GreaterOrEqualFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal >= bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(EqualFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal == bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NotEqualFloat) {
                float aVal = 0.0f, bVal = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(bVal))) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeBool(aVal != bVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NegateFloat) {
                const auto* vPtr = std::get_if<FloatValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(-vPtr->value);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertIntToFloatFast) {
                const auto* vPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(static_cast<float>(*vPtr));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertDoubleToFloatFast) {
                const auto* vPtr = std::get_if<double>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                const double d = *vPtr;
                if (ip->checked1()) {
                    if (RS_FLAT_UNLIKELY(!(d >= -static_cast<double>(std::numeric_limits<float>::max()) &&
                                           d <= static_cast<double>(std::numeric_limits<float>::max())))) {
                        goto L_FlatMathFallback;
                    }
                }
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot].storeFloat(static_cast<float>(d));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertFloatToIntFast) {
                const auto* vPtr = std::get_if<FloatValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                const double d = static_cast<double>(vPtr->value);
                if (RS_FLAT_UNLIKELY(!(d >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) &&
                                       d <= static_cast<double>(std::numeric_limits<std::int32_t>::max())))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = static_cast<std::int64_t>(d);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertFloatToDoubleFast) {
                const auto* vPtr = std::get_if<FloatValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = static_cast<double>(vPtr->value);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(ConvertLongToIntFast) {
                const auto* vPtr = std::get_if<LongValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr)) goto L_FlatMathFallback;
                const std::int64_t val = vPtr->value;
                if (ip->checked1()) {
                    if (RS_FLAT_UNLIKELY(val < std::numeric_limits<std::int32_t>::min() ||
                                         val > std::numeric_limits<std::int32_t>::max())) {
                        goto L_FlatMathFallback;
                    }
                    RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                    frameBase[ip->dstSlot] = val;
                } else {
                    RS_FLAT_SPAN_CHECK();
                localExecuted += spanCost;
                    frameBase[ip->dstSlot] = wrapInt32(val);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperConstFloat) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float fImm = 0.0f;
                std::memcpy(&fImm, &ip->integerImmediate, sizeof(float));
                localExecuted += 2;
                frameBase[ip->dstSlot].storeFloat(fImm);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperConstFloatStoreLocal) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float fImm = 0.0f;
                std::memcpy(&fImm, &ip->integerImmediate, sizeof(float));
                localExecuted += 3;
                frameBase[ip->storeSlot].storeFloat(fImm);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatLocalStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->dstSlot].storeFloat(left);
                frameBase[ip->aux0Slot].storeFloat(right);
                frameBase[ip->aux1Slot].storeFloat(out);
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatLocalStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 4u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 4;
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatReg) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                frameBase[ip->dstSlot].storeFloat(left);
                frameBase[ip->aux1Slot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatRegElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                frameBase[ip->aux1Slot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatRegStore) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 3;
                frameBase[ip->dstSlot].storeFloat(left);
                frameBase[ip->aux1Slot].storeFloat(out);
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalFloatRegStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 3u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 3;
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryRegFloatStoreElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                float left = 0.0f, right = 0.0f;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFloat(left) ||
                                     !frameBase[ip->src1Slot].tryGetFloat(right))) goto L_FlatSuperFallback;
                float out = 0.0f;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::AddFloat: out = left + right; break;
                case detail::QuickOpcode::SubtractFloat: out = left - right; break;
                case detail::QuickOpcode::MultiplyFloat: out = left * right; break;
                case detail::QuickOpcode::DivideFloat: out = left / right; break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                frameBase[ip->storeSlot].storeFloat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(NewStructFast) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                switch (ip->aux0Slot) {
                case 1: frameBase[ip->dstSlot] = FixedValue{}; break;
                case 2: frameBase[ip->dstSlot] = Fixed16Value{}; break;
                case 3: frameBase[ip->dstSlot] = Vec2Value{}; break;
                case 4: frameBase[ip->dstSlot] = Vec3Value{}; break;
                case 5: frameBase[ip->dstSlot] = QuatValue{}; break;
                default:
                    --localExecuted;
                    goto L_FlatGenericFallback;
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadFixedRawFast) {
                const auto* fx = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!fx)) goto L_FlatMathFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += cost;
                frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(fx->raw));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreFixedRawFast) {
                const auto* fx = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                const auto* rawPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!fx || !rawPtr)) goto L_FlatGenericFallback;
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(*rawPtr));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadVec3FieldFast) {
                const auto* v3 = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!v3)) goto L_FlatMathFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += cost;
                const std::int32_t raw = (ip->aux0Slot == 0)
                    ? v3->x
                    : ((ip->aux0Slot == 1) ? v3->y : v3->z);
                frameBase[ip->dstSlot].storeFixed(raw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreVec3FieldFast) {
                const auto* v3 = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                const auto* fx = std::get_if<FixedValue>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!v3 || !fx)) goto L_FlatGenericFallback;
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                Vec3Value out = *v3;
                if (ip->aux0Slot == 0) out.x = fx->raw;
                else if (ip->aux0Slot == 1) out.y = fx->raw;
                else out.z = fx->raw;
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(LoadQuatFieldFast) {
                QuatValue q{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(q))) goto L_FlatMathFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += cost;
                std::int32_t raw = 0;
                switch (ip->aux0Slot) {
                case 0: raw = q.x; break;
                case 1: raw = q.y; break;
                case 2: raw = q.z; break;
                default: raw = q.w; break;
                }
                frameBase[ip->dstSlot].storeFixed(raw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreQuatFieldFast) {
                QuatValue out{};
                std::int32_t fxRaw = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(out) ||
                                     !frameBase[ip->src1Slot].tryGetFixed(fxRaw))) {
                    goto L_FlatGenericFallback;
                }
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                switch (ip->aux0Slot) {
                case 0: out.x = fxRaw; break;
                case 1: out.y = fxRaw; break;
                case 2: out.z = fxRaw; break;
                default: out.w = fxRaw; break;
                }
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            #define RS_FLAT_MATH_CALL_ACCOUNTING() \
                const std::uint64_t mathCost = ip->aux0Slot; \
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu; \
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu; \
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || \
                                     mathCost > budgetLimit - localExecuted || \
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) { \
                    goto L_FlatMathFallback; \
                } \
                localExecuted += mathCost; \
                state.statistics.functionCalls += mathCalls; \
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) { \
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth; \
                } \
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc()

            RS_FLAT_CASE(MathFixedConst) {
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(ip->integerImmediate));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedFromRaw) {
                const auto* aPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !isValidInt32Payload(*aPtr))) goto L_FlatMathFallback;
                const std::int32_t raw = static_cast<std::int32_t>(*aPtr);
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(raw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedFromInt) {
                const auto* aPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || *aPtr < -8388608 || *aPtr > 8388607)) {
                    goto L_FlatMathFallback;
                }
                const std::int32_t scaled = static_cast<std::int32_t>(*aPtr * 256);
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(scaled);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedRatio) {
                const auto* numPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                const auto* denPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(
                        !numPtr || !denPtr || *denPtr == 0 ||
                        !isValidInt32Payload(*numPtr) || !isValidInt32Payload(*denPtr))) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t res = (*numPtr * 256) / *denPtr;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedNeg) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || aPtr->raw == std::numeric_limits<std::int32_t>::min())) {
                    goto L_FlatMathFallback;
                }
                const std::int32_t neg = -aPtr->raw;
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(neg);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedAdd) {
                std::int32_t aRaw = 0, bRaw = 0;
                const bool okFx = frameBase[ip->src0Slot].tryGetFixed(aRaw) && frameBase[ip->src1Slot].tryGetFixed(bRaw);
                if (RS_FLAT_UNLIKELY(!okFx)) goto L_FlatMathFallback;
                const std::int64_t res =
                    static_cast<std::int64_t>(aRaw) + static_cast<std::int64_t>(bRaw);
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedSub) {
                std::int32_t aRaw = 0, bRaw = 0;
                const bool okFx = frameBase[ip->src0Slot].tryGetFixed(aRaw) && frameBase[ip->src1Slot].tryGetFixed(bRaw);
                if (RS_FLAT_UNLIKELY(!okFx)) goto L_FlatMathFallback;
                const std::int64_t res =
                    static_cast<std::int64_t>(aRaw) - static_cast<std::int64_t>(bRaw);
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedMul) {
                std::int32_t aRaw = 0, bRaw = 0;
                const bool okFx = frameBase[ip->src0Slot].tryGetFixed(aRaw) && frameBase[ip->src1Slot].tryGetFixed(bRaw);
                if (RS_FLAT_UNLIKELY(!okFx)) goto L_FlatMathFallback;
                const std::int64_t res =
                    (static_cast<std::int64_t>(aRaw) * static_cast<std::int64_t>(bRaw)) / 256;
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedMulInt) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr || !isValidInt32Payload(*bPtr))) goto L_FlatMathFallback;
                const std::int64_t res = static_cast<std::int64_t>(aPtr->raw) * (*bPtr);
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedDiv) {
                std::int32_t aRaw = 0, bRaw = 0;
                const bool okFx = frameBase[ip->src0Slot].tryGetFixed(aRaw) && frameBase[ip->src1Slot].tryGetFixed(bRaw);
                if (RS_FLAT_UNLIKELY(!okFx || bRaw == 0)) goto L_FlatMathFallback;
                const std::int64_t res =
                    (static_cast<std::int64_t>(aRaw) * 256) / static_cast<std::int64_t>(bRaw);
                if (RS_FLAT_UNLIKELY(res < std::numeric_limits<std::int32_t>::min() ||
                                     res > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(res));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedDivInt) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr || *bPtr == 0 || !isValidInt32Payload(*bPtr) ||
                                     (aPtr->raw == std::numeric_limits<std::int32_t>::min() && *bPtr == -1))) {
                    goto L_FlatMathFallback;
                }
                const std::int32_t res = static_cast<std::int32_t>(aPtr->raw / *bPtr);
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(res);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedSin) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr)) goto L_FlatMathFallback;
                std::uint64_t extraInst = 0;
                std::uint64_t branches = 0;
                const std::int32_t outRaw = fastEvalFixedSin(
                    aPtr->raw,
                    static_cast<std::uint64_t>(ip->integerImmediate),
                    extraInst,
                    branches);
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + extraInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeFixed(outRaw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedCos) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr)) goto L_FlatMathFallback;
                const std::int64_t shifted = static_cast<std::int64_t>(aPtr->raw) + 402LL;
                if (RS_FLAT_UNLIKELY(!isValidInt32Payload(shifted))) goto L_FlatMathFallback;
                std::uint64_t extraInst = 0;
                std::uint64_t branches = 0;
                const std::int32_t outRaw = fastEvalFixedSin(
                    static_cast<std::int32_t>(shifted),
                    static_cast<std::uint64_t>(ip->integerImmediate),
                    extraInst,
                    branches);
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + extraInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeFixed(outRaw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathFixedSqrt) {
                const auto* aPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr)) goto L_FlatMathFallback;
                const std::size_t baseCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                const auto packed = static_cast<std::uint64_t>(ip->integerImmediate);
                const std::uint64_t loopIterCost = packed & 0xFFu;
                const std::uint64_t negCost = (packed >> 8u) & 0xFFFFu;
                std::uint64_t mathCost = 0;
                std::size_t mathCalls = 0;
                std::uint64_t branches = 0;
                std::int32_t outRaw = 0;
                if (aPtr->raw <= 0) {
                    mathCost = negCost;
                    mathCalls = baseCalls - 1u;
                    branches = 1;
                } else {
                    std::uint64_t loopInst = 0;
                    std::uint64_t sqBr = 0;
                    const std::int64_t ans = fastEvalSqrt64Pos(
                        static_cast<std::int64_t>(aPtr->raw) * 256LL,
                        loopIterCost,
                        loopInst,
                        sqBr);
                    outRaw = static_cast<std::int32_t>(ans);
                    mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                    mathCalls = baseCalls;
                    branches = 1u + sqBr;
                }
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeFixed(outRaw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathSqrt64) {
                const auto* aPtr = std::get_if<LongValue>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr)) goto L_FlatMathFallback;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                const auto packed = static_cast<std::uint64_t>(ip->integerImmediate);
                const std::uint64_t loopIterCost = packed & 0xFFu;
                const std::uint64_t negCost = (packed >> 8u) & 0xFFFFu;
                std::uint64_t mathCost = 0;
                std::uint64_t branches = 0;
                std::int64_t outVal = 0;
                if (aPtr->value <= 0) {
                    mathCost = negCost;
                    branches = 1;
                } else {
                    std::uint64_t loopInst = 0;
                    outVal = fastEvalSqrt64Pos(aPtr->value, loopIterCost, loopInst, branches);
                    mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                }
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeLong(outVal);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Const) {
                RS_FLAT_MATH_CALL_ACCOUNTING();
                const auto c = static_cast<std::int32_t>(ip->integerImmediate);
                frameBase[ip->dstSlot].storeVec3(c, c, c);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Raw) {
                const auto* xPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                const auto* yPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                const auto* zPtr = std::get_if<std::int64_t>(&frameBase[ip->src2Slot]);
                if (RS_FLAT_UNLIKELY(
                        !xPtr || !yPtr || !zPtr ||
                        !isValidInt32Payload(*xPtr) ||
                        !isValidInt32Payload(*yPtr) ||
                        !isValidInt32Payload(*zPtr))) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(*xPtr),
                    static_cast<std::int32_t>(*yPtr),
                    static_cast<std::int32_t>(*zPtr)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Of) {
                const auto* xPtr = std::get_if<std::int64_t>(&frameBase[ip->src0Slot]);
                const auto* yPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                const auto* zPtr = std::get_if<std::int64_t>(&frameBase[ip->src2Slot]);
                if (RS_FLAT_UNLIKELY(
                        !xPtr || !yPtr || !zPtr ||
                        *xPtr < -8388608 || *xPtr > 8388607 ||
                        *yPtr < -8388608 || *yPtr > 8388607 ||
                        *zPtr < -8388608 || *zPtr > 8388607)) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(*xPtr * 256),
                    static_cast<std::int32_t>(*yPtr * 256),
                    static_cast<std::int32_t>(*zPtr * 256)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Neg) {
                const auto* vPtr = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr ||
                                     vPtr->x == std::numeric_limits<std::int32_t>::min() ||
                                     vPtr->y == std::numeric_limits<std::int32_t>::min() ||
                                     vPtr->z == std::numeric_limits<std::int32_t>::min())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{-vPtr->x, -vPtr->y, -vPtr->z};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Add) {
                Vec3Value aVal{}, bVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(bVal))) goto L_FlatMathFallback;
                const std::int64_t rx = static_cast<std::int64_t>(aVal.x) + bVal.x;
                const std::int64_t ry = static_cast<std::int64_t>(aVal.y) + bVal.y;
                const std::int64_t rz = static_cast<std::int64_t>(aVal.z) + bVal.z;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Sub) {
                Vec3Value aVal{}, bVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(bVal))) goto L_FlatMathFallback;
                const std::int64_t rx = static_cast<std::int64_t>(aVal.x) - bVal.x;
                const std::int64_t ry = static_cast<std::int64_t>(aVal.y) - bVal.y;
                const std::int64_t rz = static_cast<std::int64_t>(aVal.z) - bVal.z;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3MulFixed) {
                Vec3Value vVal{};
                std::int32_t sRaw = 0;
                const bool okOps = frameBase[ip->src0Slot].tryGetVec3(vVal) && frameBase[ip->src1Slot].tryGetFixed(sRaw);
                if (RS_FLAT_UNLIKELY(!okOps)) goto L_FlatMathFallback;
                const std::int64_t sr = sRaw;
                const std::int64_t rx = (static_cast<std::int64_t>(vVal.x) * sr) / 256;
                const std::int64_t ry = (static_cast<std::int64_t>(vVal.y) * sr) / 256;
                const std::int64_t rz = (static_cast<std::int64_t>(vVal.z) * sr) / 256;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3MulInt) {
                const auto* vPtr = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                const auto* sPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr || !sPtr || !isValidInt32Payload(*sPtr))) goto L_FlatMathFallback;
                const std::int64_t s = *sPtr;
                const std::int64_t rx = static_cast<std::int64_t>(vPtr->x) * s;
                const std::int64_t ry = static_cast<std::int64_t>(vPtr->y) * s;
                const std::int64_t rz = static_cast<std::int64_t>(vPtr->z) * s;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3DivFixed) {
                Vec3Value vVal{};
                std::int32_t sRaw = 0;
                const bool okOps = frameBase[ip->src0Slot].tryGetVec3(vVal) && frameBase[ip->src1Slot].tryGetFixed(sRaw);
                if (RS_FLAT_UNLIKELY(!okOps || sRaw == 0)) goto L_FlatMathFallback;
                const std::int64_t sr = sRaw;
                const std::int64_t rx = (static_cast<std::int64_t>(vVal.x) * 256) / sr;
                const std::int64_t ry = (static_cast<std::int64_t>(vVal.y) * 256) / sr;
                const std::int64_t rz = (static_cast<std::int64_t>(vVal.z) * 256) / sr;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max() ||
                                     rz < std::numeric_limits<std::int32_t>::min() || rz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(rx),
                    static_cast<std::int32_t>(ry),
                    static_cast<std::int32_t>(rz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3DivInt) {
                const auto* vPtr = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                const auto* sPtr = std::get_if<std::int64_t>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr || !sPtr || *sPtr == 0 || !isValidInt32Payload(*sPtr))) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t s = *sPtr;
                if (RS_FLAT_UNLIKELY(s == -1 &&
                                     (vPtr->x == std::numeric_limits<std::int32_t>::min() ||
                                      vPtr->y == std::numeric_limits<std::int32_t>::min() ||
                                      vPtr->z == std::numeric_limits<std::int32_t>::min()))) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(vPtr->x / s),
                    static_cast<std::int32_t>(vPtr->y / s),
                    static_cast<std::int32_t>(vPtr->z / s)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Dot) {
                Vec3Value aVal{}, bVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(bVal))) goto L_FlatMathFallback;
                const std::int64_t px = (static_cast<std::int64_t>(aVal.x) * bVal.x) / 256;
                const std::int64_t py = (static_cast<std::int64_t>(aVal.y) * bVal.y) / 256;
                const std::int64_t pz = (static_cast<std::int64_t>(aVal.z) * bVal.z) / 256;
                if (RS_FLAT_UNLIKELY(px < std::numeric_limits<std::int32_t>::min() || px > std::numeric_limits<std::int32_t>::max() ||
                                     py < std::numeric_limits<std::int32_t>::min() || py > std::numeric_limits<std::int32_t>::max() ||
                                     pz < std::numeric_limits<std::int32_t>::min() || pz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t xy = px + py;
                if (RS_FLAT_UNLIKELY(xy < std::numeric_limits<std::int32_t>::min() || xy > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t xyz = xy + pz;
                if (RS_FLAT_UNLIKELY(xyz < std::numeric_limits<std::int32_t>::min() || xyz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(xyz));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec3Cross) {
                Vec3Value aVal{}, bVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(aVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(bVal))) goto L_FlatMathFallback;
                const std::int64_t p1 = (static_cast<std::int64_t>(aVal.y) * bVal.z) / 256;
                const std::int64_t p2 = (static_cast<std::int64_t>(aVal.z) * bVal.y) / 256;
                const std::int64_t p3 = (static_cast<std::int64_t>(aVal.z) * bVal.x) / 256;
                const std::int64_t p4 = (static_cast<std::int64_t>(aVal.x) * bVal.z) / 256;
                const std::int64_t p5 = (static_cast<std::int64_t>(aVal.x) * bVal.y) / 256;
                const std::int64_t p6 = (static_cast<std::int64_t>(aVal.y) * bVal.x) / 256;
                if (RS_FLAT_UNLIKELY(p1 < std::numeric_limits<std::int32_t>::min() || p1 > std::numeric_limits<std::int32_t>::max() ||
                                     p2 < std::numeric_limits<std::int32_t>::min() || p2 > std::numeric_limits<std::int32_t>::max() ||
                                     p3 < std::numeric_limits<std::int32_t>::min() || p3 > std::numeric_limits<std::int32_t>::max() ||
                                     p4 < std::numeric_limits<std::int32_t>::min() || p4 > std::numeric_limits<std::int32_t>::max() ||
                                     p5 < std::numeric_limits<std::int32_t>::min() || p5 > std::numeric_limits<std::int32_t>::max() ||
                                     p6 < std::numeric_limits<std::int32_t>::min() || p6 > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t cx = p1 - p2;
                const std::int64_t cy = p3 - p4;
                const std::int64_t cz = p5 - p6;
                if (RS_FLAT_UNLIKELY(cx < std::numeric_limits<std::int32_t>::min() || cx > std::numeric_limits<std::int32_t>::max() ||
                                     cy < std::numeric_limits<std::int32_t>::min() || cy > std::numeric_limits<std::int32_t>::max() ||
                                     cz < std::numeric_limits<std::int32_t>::min() || cz > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec3Value out{
                    static_cast<std::int32_t>(cx),
                    static_cast<std::int32_t>(cy),
                    static_cast<std::int32_t>(cz)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec2Add) {
                const auto* aPtr = std::get_if<Vec2Value>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<Vec2Value>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr)) goto L_FlatMathFallback;
                const std::int64_t rx = static_cast<std::int64_t>(aPtr->x) + bPtr->x;
                const std::int64_t ry = static_cast<std::int64_t>(aPtr->y) + bPtr->y;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec2Value out{static_cast<std::int32_t>(rx), static_cast<std::int32_t>(ry)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec2(out.x, out.y);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec2Sub) {
                const auto* aPtr = std::get_if<Vec2Value>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<Vec2Value>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr)) goto L_FlatMathFallback;
                const std::int64_t rx = static_cast<std::int64_t>(aPtr->x) - bPtr->x;
                const std::int64_t ry = static_cast<std::int64_t>(aPtr->y) - bPtr->y;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec2Value out{static_cast<std::int32_t>(rx), static_cast<std::int32_t>(ry)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec2(out.x, out.y);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec2MulFixed) {
                const auto* vPtr = std::get_if<Vec2Value>(&frameBase[ip->src0Slot]);
                const auto* sPtr = std::get_if<FixedValue>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!vPtr || !sPtr)) goto L_FlatMathFallback;
                const std::int64_t sr = sPtr->raw;
                const std::int64_t rx = (static_cast<std::int64_t>(vPtr->x) * sr) / 256;
                const std::int64_t ry = (static_cast<std::int64_t>(vPtr->y) * sr) / 256;
                if (RS_FLAT_UNLIKELY(rx < std::numeric_limits<std::int32_t>::min() || rx > std::numeric_limits<std::int32_t>::max() ||
                                     ry < std::numeric_limits<std::int32_t>::min() || ry > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const Vec2Value out{static_cast<std::int32_t>(rx), static_cast<std::int32_t>(ry)};
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec2(out.x, out.y);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathVec2Dot) {
                const auto* aPtr = std::get_if<Vec2Value>(&frameBase[ip->src0Slot]);
                const auto* bPtr = std::get_if<Vec2Value>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!aPtr || !bPtr)) goto L_FlatMathFallback;
                const std::int64_t px = (static_cast<std::int64_t>(aPtr->x) * bPtr->x) / 256;
                const std::int64_t py = (static_cast<std::int64_t>(aPtr->y) * bPtr->y) / 256;
                if (RS_FLAT_UNLIKELY(px < std::numeric_limits<std::int32_t>::min() || px > std::numeric_limits<std::int32_t>::max() ||
                                     py < std::numeric_limits<std::int32_t>::min() || py > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t xy = px + py;
                if (RS_FLAT_UNLIKELY(xy < std::numeric_limits<std::int32_t>::min() || xy > std::numeric_limits<std::int32_t>::max())) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(static_cast<std::int32_t>(xy));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatIdentity) {
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeQuat(QuatValue{0, 0, 0, 256});
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatDot) {
                QuatValue a{}, b{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b))) {
                    goto L_FlatMathFallback;
                }
                std::int32_t dotRaw = 0;
                if (RS_FLAT_UNLIKELY(!fastQuatDot(a, b, dotRaw))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(dotRaw);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatMul) {
                QuatValue a{}, b{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b))) {
                    goto L_FlatMathFallback;
                }
                QuatValue out{};
                std::int32_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, s0 = 0, s1 = 0;
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(a.w, b.x, t0) && fastFixedMul(a.x, b.w, t1) && fastFixedAdd(t0, t1, s0) &&
                          fastFixedMul(a.y, b.z, t2) && fastFixedAdd(s0, t2, s1) &&
                          fastFixedMul(a.z, b.y, t3) && fastFixedSub(s1, t3, out.x)))) {
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(a.w, b.y, t0) && fastFixedMul(a.x, b.z, t1) && fastFixedSub(t0, t1, s0) &&
                          fastFixedMul(a.y, b.w, t2) && fastFixedAdd(s0, t2, s1) &&
                          fastFixedMul(a.z, b.x, t3) && fastFixedAdd(s1, t3, out.y)))) {
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(a.w, b.z, t0) && fastFixedMul(a.x, b.y, t1) && fastFixedAdd(t0, t1, s0) &&
                          fastFixedMul(a.y, b.x, t2) && fastFixedSub(s0, t2, s1) &&
                          fastFixedMul(a.z, b.w, t3) && fastFixedAdd(s1, t3, out.z)))) {
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(a.w, b.w, t0) && fastFixedMul(a.x, b.x, t1) && fastFixedSub(t0, t1, s0) &&
                          fastFixedMul(a.y, b.y, t2) && fastFixedSub(s0, t2, s1) &&
                          fastFixedMul(a.z, b.z, t3) && fastFixedSub(s1, t3, out.w)))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatRotate) {
                const auto* ref = std::get_if<ObjectRef>(&frameBase[ip->src0Slot]);
                const auto* vPtr = std::get_if<Vec3Value>(&frameBase[ip->src1Slot]);
                if (RS_FLAT_UNLIKELY(!ref || ref->kind != ObjectKind::Record || !vPtr || !state.heap)) {
                    goto L_FlatMathFallback;
                }
                semantic::SymbolId actualTid = 0;
                const Value* fPtr = state.heap->fieldGetFast(*ref, 0, &actualTid);
                QuatValue qVal{};
                if (RS_FLAT_UNLIKELY(!fPtr || !fPtr->tryGetQuat(qVal))) goto L_FlatMathFallback;
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(!fastQuatRotateVec3(qVal, *vPtr, out))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatRotateVec3) {
                QuatValue qVal{};
                Vec3Value vVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(qVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(vVal))) {
                    goto L_FlatMathFallback;
                }
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(!fastQuatRotateVec3(qVal, vVal, out))) {
                    goto L_FlatMathFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatRotateBoxed) {
                QuatValue qVal{};
                Vec3Value vVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(qVal) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(vVal))) {
                    goto L_FlatSuperFallback;
                }
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(!fastQuatRotateVec3(qVal, vVal, out))) {
                    goto L_FlatSuperFallback;
                }
                const std::uint64_t mathCost = ip->aux0Slot;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->storeSlot != detail::InvalidCompactReg) {
                    frameBase[ip->storeSlot].storeQuat(qVal);
                }
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatNormalize) {
                QuatValue qVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(qVal))) goto L_FlatMathFallback;
                QuatValue out{};
                std::uint64_t loopInst = 0;
                std::uint64_t branches = 0;
                if (RS_FLAT_UNLIKELY(!fastQuatNormalize(
                        qVal,
                        static_cast<std::uint64_t>(ip->integerImmediate),
                        out,
                        loopInst,
                        branches))) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatNormalizedBoxed) {
                QuatValue qVal{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(qVal))) goto L_FlatSuperFallback;
                QuatValue out{};
                std::uint64_t loopInst = 0;
                std::uint64_t branches = 0;
                if (RS_FLAT_UNLIKELY(!fastQuatNormalize(
                        qVal,
                        static_cast<std::uint64_t>(ip->integerImmediate),
                        out,
                        loopInst,
                        branches))) {
                    goto L_FlatSuperFallback;
                }
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->storeSlot != detail::InvalidCompactReg) {
                    frameBase[ip->storeSlot].storeQuat(qVal);
                }
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatFromEuler) {
                const auto* pPtr = std::get_if<FixedValue>(&frameBase[ip->src0Slot]);
                const auto* yPtr = std::get_if<FixedValue>(&frameBase[ip->src1Slot]);
                const auto* rPtr = std::get_if<FixedValue>(&frameBase[ip->src2Slot]);
                if (RS_FLAT_UNLIKELY(!pPtr || !yPtr || !rPtr)) goto L_FlatMathFallback;
                const std::int32_t hp = pPtr->raw / 2;
                const std::int32_t hy = yPtr->raw / 2;
                const std::int32_t hr = rPtr->raw / 2;
                const std::uint64_t wrapExtra = static_cast<std::uint64_t>(ip->integerImmediate);
                std::uint64_t extraInst = 0;
                std::uint64_t branches = 0;
                const std::int32_t sp = fastEvalFixedSin(hp, wrapExtra, extraInst, branches);
                const std::int32_t cp = fastEvalFixedSin(hp + 402, wrapExtra, extraInst, branches);
                const std::int32_t sy = fastEvalFixedSin(hy, wrapExtra, extraInst, branches);
                const std::int32_t cy = fastEvalFixedSin(hy + 402, wrapExtra, extraInst, branches);
                const std::int32_t sr = fastEvalFixedSin(hr, wrapExtra, extraInst, branches);
                const std::int32_t cr = fastEvalFixedSin(hr + 402, wrapExtra, extraInst, branches);

                QuatValue out{};
                std::int32_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(cy, sp, a0) && fastFixedMul(a0, cr, a1) &&
                          fastFixedMul(sy, cp, b0) && fastFixedMul(b0, sr, b1) &&
                          fastFixedAdd(a1, b1, out.x) &&
                          fastFixedMul(sy, cp, a0) && fastFixedMul(a0, cr, a1) &&
                          fastFixedMul(cy, sp, b0) && fastFixedMul(b0, sr, b1) &&
                          fastFixedSub(a1, b1, out.y) &&
                          fastFixedMul(cy, cp, a0) && fastFixedMul(a0, sr, a1) &&
                          fastFixedMul(sy, sp, b0) && fastFixedMul(b0, cr, b1) &&
                          fastFixedSub(a1, b1, out.z) &&
                          fastFixedMul(cy, cp, a0) && fastFixedMul(a0, cr, a1) &&
                          fastFixedMul(sy, sp, b0) && fastFixedMul(b0, sr, b1) &&
                          fastFixedAdd(a1, b1, out.w)))) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + extraInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(MathQuatLerp) {
                QuatValue a{}, target{};
                std::int32_t tRaw = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(target) ||
                                     !frameBase[ip->src2Slot].tryGetFixed(tRaw))) {
                    goto L_FlatMathFallback;
                }
                std::int32_t dotRaw = 0;
                if (RS_FLAT_UNLIKELY(!fastQuatDot(a, target, dotRaw))) {
                    goto L_FlatMathFallback;
                }
                const auto packed = static_cast<std::uint64_t>(ip->integerImmediate);
                const std::uint64_t loopIterCost = packed & 0xFFu;
                const std::uint64_t dotNegExtraCost = (packed >> 8u) & 0xFFFFu;
                const std::size_t dotNegExtraCalls = static_cast<std::size_t>((packed >> 24u) & 0xFFu);
                std::uint64_t extraInst = 0;
                std::size_t extraCalls = 0;
                std::uint64_t branches = 2; // Lerp dot<0 branch + Normalized len<=0 branch
                if (dotRaw < 0) {
                    if (RS_FLAT_UNLIKELY(
                            !(fastFixedNeg(target.x, target.x) &&
                              fastFixedNeg(target.y, target.y) &&
                              fastFixedNeg(target.z, target.z) &&
                              fastFixedNeg(target.w, target.w)))) {
                        goto L_FlatMathFallback;
                    }
                    extraInst += dotNegExtraCost;
                    extraCalls += dotNegExtraCalls;
                    ++branches; // bb1 unconditional jump -> bb2
                }
                QuatValue res{};
                std::int32_t dx = 0, dy = 0, dz = 0, dw = 0;
                std::int32_t sx = 0, sy = 0, sz = 0, sw = 0;
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedSub(target.x, a.x, dx) && fastFixedMul(dx, tRaw, sx) && fastFixedAdd(a.x, sx, res.x) &&
                          fastFixedSub(target.y, a.y, dy) && fastFixedMul(dy, tRaw, sy) && fastFixedAdd(a.y, sy, res.y) &&
                          fastFixedSub(target.z, a.z, dz) && fastFixedMul(dz, tRaw, sz) && fastFixedAdd(a.z, sz, res.z) &&
                          fastFixedSub(target.w, a.w, dw) && fastFixedMul(dw, tRaw, sw) && fastFixedAdd(a.w, sw, res.w)))) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t xx = static_cast<std::int64_t>(res.x) * res.x;
                const std::int64_t yy = static_cast<std::int64_t>(res.y) * res.y;
                const std::int64_t zz = static_cast<std::int64_t>(res.z) * res.z;
                const std::int64_t ww = static_cast<std::int64_t>(res.w) * res.w;
                if (RS_FLAT_UNLIKELY(
                        xx > std::numeric_limits<std::int64_t>::max() - yy ||
                        xx + yy > std::numeric_limits<std::int64_t>::max() - zz ||
                        xx + yy + zz > std::numeric_limits<std::int64_t>::max() - ww)) {
                    goto L_FlatMathFallback;
                }
                const std::int64_t r2 = xx + yy + zz + ww;
                if (RS_FLAT_UNLIKELY(r2 <= 0)) {
                    goto L_FlatMathFallback;
                }
                std::uint64_t loopInst = 0;
                std::uint64_t sqBranches = 0;
                const std::int64_t len64 = fastEvalSqrt64Pos(r2, loopIterCost, loopInst, sqBranches);
                if (RS_FLAT_UNLIKELY(!isValidInt32Payload(len64) || len64 <= 0)) {
                    goto L_FlatMathFallback;
                }
                const auto lenRaw = static_cast<std::int32_t>(len64);
                QuatValue out{};
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedDiv(res.x, lenRaw, out.x) &&
                          fastFixedDiv(res.y, lenRaw, out.y) &&
                          fastFixedDiv(res.z, lenRaw, out.z) &&
                          fastFixedDiv(res.w, lenRaw, out.w)))) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t mathCost =
                    static_cast<std::uint64_t>(ip->aux0Slot) + extraInst + loopInst;
                const std::size_t mathCalls = (ip->aux1Slot & 0xFFu) + extraCalls;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches + sqBranches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperLoadVec3RawFast) {
                const auto* v3 = std::get_if<Vec3Value>(&frameBase[ip->src0Slot]);
                if (RS_FLAT_UNLIKELY(!v3)) goto L_FlatSuperFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                const std::int32_t raw = (ip->aux0Slot == 0)
                    ? v3->x
                    : ((ip->aux0Slot == 1) ? v3->y : v3->z);
                frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(raw));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperLoadQuatRawFast) {
                QuatValue q{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(q))) goto L_FlatSuperFallback;
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                std::int32_t raw = 0;
                switch (ip->aux0Slot) {
                case 0: raw = q.x; break;
                case 1: raw = q.y; break;
                case 2: raw = q.z; break;
                default: raw = q.w; break;
                }
                frameBase[ip->dstSlot].storeInt64(static_cast<std::int64_t>(raw));
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperFixedMulAdd) {
                std::int32_t aRaw = 0, bRaw = 0, cRaw = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetFixed(aRaw) ||
                                     !frameBase[ip->src1Slot].tryGetFixed(bRaw) ||
                                     !frameBase[ip->src2Slot].tryGetFixed(cRaw))) goto L_FlatSuperFallback;
                std::int32_t prod = 0;
                std::int32_t sum = 0;
                if (RS_FLAT_UNLIKELY(!fastFixedMul(bRaw, cRaw, prod) ||
                                     !fastFixedAdd(aRaw, prod, sum))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeFixed(sum);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperVec3MulAdd) {
                Vec3Value a{}, b{};
                std::int32_t sr = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(a) ||
                                     !frameBase[ip->src1Slot].tryGetVec3(b) ||
                                     !frameBase[ip->src2Slot].tryGetFixed(sr))) goto L_FlatSuperFallback;
                Vec3Value prod{};
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(
                        !(fastFixedMul(b.x, sr, prod.x) &&
                          fastFixedMul(b.y, sr, prod.y) &&
                          fastFixedMul(b.z, sr, prod.z) &&
                          fastFixedAdd(a.x, prod.x, out.x) &&
                          fastFixedAdd(a.y, prod.y, out.y) &&
                          fastFixedAdd(a.z, prod.z, out.z)))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperVec3AddQuatRotate) {
                Vec3Value a{}, v{};
                QuatValue q{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetVec3(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(q) ||
                                     !frameBase[ip->src2Slot].tryGetVec3(v))) {
                    goto L_FlatSuperFallback;
                }
                Vec3Value rot{};
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(
                        !fastQuatRotateVec3(q, v, rot) ||
                        !(fastFixedAdd(a.x, rot.x, out.x) &&
                          fastFixedAdd(a.y, rot.y, out.y) &&
                          fastFixedAdd(a.z, rot.z, out.z)))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                if (ip->storeSlot != detail::InvalidCompactReg) {
                    frameBase[ip->storeSlot].storeQuat(q);
                }
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatMulNormalized) {
                QuatValue a{}, b{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b))) {
                    goto L_FlatSuperFallback;
                }
                QuatValue prod{};
                QuatValue out{};
                std::uint64_t loopInst = 0;
                std::uint64_t branches = 0;
                if (RS_FLAT_UNLIKELY(
                        !fastQuatMul(a, b, prod) ||
                        !fastQuatNormalize(
                            prod,
                            static_cast<std::uint64_t>(ip->integerImmediate),
                            out,
                            loopInst,
                            branches))) {
                    goto L_FlatSuperFallback;
                }
                const std::uint64_t mathCost = static_cast<std::uint64_t>(ip->aux0Slot) + loopInst;
                const std::size_t mathCalls = ip->aux1Slot & 0xFFu;
                const std::size_t mathDepth = (ip->aux1Slot >> 8u) & 0xFFu;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit ||
                                     mathCost > budgetLimit - localExecuted ||
                                     state.stack.size() + mathDepth > state.limits.recursionLimit)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += mathCost;
                state.statistics.functionCalls += mathCalls;
                state.statistics.branchesTaken += branches;
                if (state.stack.size() + mathDepth > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + mathDepth;
                }
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->storeSlot != detail::InvalidCompactReg) {
                    frameBase[ip->storeSlot].storeQuat(prod);
                }
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatMulChain) {
                QuatValue a{}, b{}, c{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b) ||
                                     !frameBase[ip->src2Slot].tryGetQuat(c))) {
                    goto L_FlatSuperFallback;
                }
                QuatValue mid{};
                QuatValue out{};
                if (RS_FLAT_UNLIKELY(!fastQuatMul(a, b, mid) || !fastQuatMul(mid, c, out))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeQuat(out);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperQuatMulRotateVec3) {
                QuatValue a{}, b{};
                Vec3Value v{};
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetQuat(a) ||
                                     !frameBase[ip->src1Slot].tryGetQuat(b) ||
                                     !frameBase[ip->src2Slot].tryGetVec3(v))) {
                    goto L_FlatSuperFallback;
                }
                QuatValue mid{};
                Vec3Value out{};
                if (RS_FLAT_UNLIKELY(!fastQuatMul(a, b, mid) || !fastQuatRotateVec3(mid, v, out))) {
                    goto L_FlatSuperFallback;
                }
                RS_FLAT_MATH_CALL_ACCOUNTING();
                frameBase[ip->dstSlot].storeVec3(out.x, out.y, out.z);
                RS_FLAT_DISPATCH_NEXT();
            }

            #undef RS_FLAT_MATH_CALL_ACCOUNTING
            #undef RS_FLAT_SPAN_CHECK

            RS_FLAT_CASE(SuperMoveParamStoreLocalElided) {
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || 2u > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += 2;
                frameBase[ip->storeSlot] = frameBase[ip->src0Slot];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntConstStoreJump) {
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) goto L_FlatSuperFallback;
                if (RS_FLAT_UNLIKELY(!tryEvaluateQuickIntNoFail(
                        ip->binaryOpcode1, ip->checked1(), left, ip->integerImmediate, frameBase[ip->dstSlot]))) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.statistics.branchesTaken +=
                    ((ip->arithFlags & detail::FlatInstruction::FlagPreJump1) != 0u) ? 2u : 1u;
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntLocalStoreJump) {
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatSuperFallback;
                if (RS_FLAT_UNLIKELY(!tryEvaluateQuickIntNoFail(
                        ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->dstSlot]))) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.statistics.branchesTaken +=
                    ((ip->arithFlags & detail::FlatInstruction::FlagPreJump1) != 0u) ? 2u : 1u;
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(SuperBinaryLocalIntRegStoreJump) {
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0, right = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left) ||
                                     !frameBase[ip->src1Slot].tryGetInt64(right))) goto L_FlatSuperFallback;
                if (RS_FLAT_UNLIKELY(!tryEvaluateQuickIntNoFail(
                        ip->binaryOpcode1, ip->checked1(), left, right, frameBase[ip->dstSlot]))) {
                    goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.statistics.branchesTaken +=
                    ((ip->arithFlags & detail::FlatInstruction::FlagPreJump1) != 0u) ? 2u : 1u;
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(SuperCheckNotNullCallPolledFast) {
                // Directly share CallPolledFast's 4-Way PIC + multi-arg + TrivialMethodKind fast path!
                goto L_CallPolledFastBody;
            }

            RS_FLAT_CASE(InlineCallEnter) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(state.stack.size() >= state.limits.recursionLimit)) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::RecursionLimitExceeded, "recursion limit exceeded");
                    goto L_FlatError;
                }
                ++state.statistics.functionCalls;
                if (state.stack.size() + 1 > state.statistics.maximumCallDepth) {
                    state.statistics.maximumCallDepth = state.stack.size() + 1;
                }
                Value* const calleeBase = frameBase + ip->dstSlot;
                const std::uint32_t nArgs = ip->inlineArgCount;
                const std::uint32_t nLocals = ip->aux0Slot;
                if ((ip->arithFlags & detail::FlatInstruction::FlagFastParamPrologue) != 0u &&
                    RS_FLAT_LIKELY(2u * nArgs <= budgetLimit - localExecuted)) {
                    Value* const calleeLocals = calleeBase + nArgs;
                    if (nArgs >= 1u) calleeLocals[0] = frameBase[ip->src0Slot];
                    if (nArgs >= 2u) calleeLocals[1] = frameBase[ip->src1Slot];
                    if (nArgs >= 3u) calleeLocals[2] = frameBase[ip->src2Slot];
                    if (nArgs >= 4u) calleeLocals[3] = frameBase[ip->aux1Slot];
                    if (nLocals > nArgs) {
                        std::memset(static_cast<void*>(calleeLocals + nArgs), 0, (nLocals - nArgs) * sizeof(Value));
                    }
                    localExecuted += 2u * nArgs;
                    ip += 1u + nArgs;
                    RS_FLAT_DISPATCH_AT_IP();
                }
                if (nArgs >= 1u) calleeBase[0] = frameBase[ip->src0Slot];
                if (nArgs >= 2u) calleeBase[1] = frameBase[ip->src1Slot];
                if (nArgs >= 3u) calleeBase[2] = frameBase[ip->src2Slot];
                if (nArgs >= 4u) calleeBase[3] = frameBase[ip->aux1Slot];
                if (nLocals != 0u) {
                    std::memset(static_cast<void*>(calleeBase + nArgs), 0, nLocals * sizeof(Value));
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InlineReturnVoid) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->integerImmediate != 0) {
                    const auto packedClear = static_cast<std::uint64_t>(ip->integerImmediate);
                    std::memset(static_cast<void*>(frameBase + (packedClear & 0xFFFFu)), 0,
                        static_cast<std::size_t>(packedClear >> 16u) * sizeof(Value));
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InlineReturnVoidJump) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (ip->integerImmediate != 0) {
                    const auto packedClear = static_cast<std::uint64_t>(ip->integerImmediate);
                    std::memset(static_cast<void*>(frameBase + (packedClear & 0xFFFFu)), 0,
                        static_cast<std::size_t>(packedClear >> 16u) * sizeof(Value));
                }
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(InlineReturnScalar) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (RS_FLAT_UNLIKELY(ip->integerImmediate != 0)) {
                    const Value retVal = frameBase[ip->src0Slot];
                    const auto packedClear = static_cast<std::uint64_t>(ip->integerImmediate);
                    std::memset(static_cast<void*>(frameBase + (packedClear & 0xFFFFu)), 0,
                        static_cast<std::size_t>(packedClear >> 16u) * sizeof(Value));
                    if (ip->dstSlot != detail::InvalidCompactReg) {
                        frameBase[ip->dstSlot] = retVal;
                    }
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = frameBase[ip->src0Slot];
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InlineReturnScalarJump) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                if (RS_FLAT_UNLIKELY(ip->integerImmediate != 0)) {
                    const Value retVal = frameBase[ip->src0Slot];
                    const auto packedClear = static_cast<std::uint64_t>(ip->integerImmediate);
                    std::memset(static_cast<void*>(frameBase + (packedClear & 0xFFFFu)), 0,
                        static_cast<std::size_t>(packedClear >> 16u) * sizeof(Value));
                    if (ip->dstSlot != detail::InvalidCompactReg) {
                        frameBase[ip->dstSlot] = retVal;
                    }
                    ip = streamBase + ip->trueTargetOffset;
                    RS_FLAT_DISPATCH_AT_IP();
                }
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = frameBase[ip->src0Slot];
                }
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(LoadElementTypedFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const auto expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1));
                const auto expectedTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (RS_FLAT_UNLIKELY(ce.view.elementType != expectedType ||
                                     ce.view.elementTypeId != expectedTid)) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot] = ce.view.data[uIdx];
                    RS_FLAT_DISPATCH_NEXT();
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                frameBase[ip->dstSlot] = ce.view.data[static_cast<std::size_t>(idx)];
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreElementPrimFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const auto expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1));
                const auto expectedTag = static_cast<ValueTag>(static_cast<std::uint8_t>(ip->binaryOpcode2));
                const Value& storeVal = frameBase[ip->src2Slot];
                if (RS_FLAT_UNLIKELY(ce.view.elementType != expectedType ||
                                     ce.view.elementTypeId != 0 ||
                                     storeVal.rawTag() != expectedTag)) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    const auto uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                    localExecuted += spanCost;
                    ce.view.data[uIdx] = storeVal;
                    RS_FLAT_DISPATCH_NEXT();
                }
                std::int64_t idx = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                     static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                ce.view.data[static_cast<std::size_t>(idx)] = storeVal;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreElementStructFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const auto expectedTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (RS_FLAT_UNLIKELY(ce.view.elementType != semantic::PrimitiveType::Struct ||
                                     ce.view.elementTypeId != expectedTid)) {
                    goto L_FlatMathFallback;
                }
                const Value& storeVal = frameBase[ip->src2Slot];
                const ValueTag vtag = storeVal.rawTag();
                bool structOk = false;
                bool needsPoolPin = false;
                bool mayHaveRefs = false;
                switch (ip->aux0Slot) {
                case 1: structOk = (vtag == ValueTag::Fixed); break;
                case 2: structOk = (vtag == ValueTag::Fixed16); break;
                case 3: structOk = (vtag == ValueTag::Vec2); break;
                case 4: structOk = (vtag == ValueTag::Vec3); break;
                case 5:
                    if (vtag == ValueTag::QuatInline) {
                        structOk = true;
                    } else if (vtag == ValueTag::QuatArena) {
                        structOk = true;
                        needsPoolPin = true;
                    }
                    break;
                default:
                    if (vtag == ValueTag::Struct &&
                        storeVal.u_.structVal.valid() &&
                        storeVal.u_.structVal.typeId == expectedTid) {
                        structOk = true;
                        needsPoolPin = true;
                        mayHaveRefs = !storeVal.u_.structVal.isInline && storeVal.u_.structVal.storage != nullptr;
                    }
                    break;
                }
                if (RS_FLAT_UNLIKELY(!structOk)) goto L_FlatMathFallback;

                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                std::size_t uIdx = 0;
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                } else {
                    std::int64_t idx = 0;
                    if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                         static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                        goto L_FlatMathFallback;
                    }
                    uIdx = static_cast<std::size_t>(idx);
                }
                if (needsPoolPin) {
                    pinValueInPools(storeVal);
                }
                if (mayHaveRefs) {
                    if (ce.view.dirtyCards != nullptr) {
                        ce.view.dirtyCards[uIdx >> ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (RS_FLAT_UNLIKELY(ce.view.needsRememberedBarrier || gcActive || state.heap->phase() != GcPhase::Idle)) {
                        state.heap->recordArrayWriteBarrier(ce.ref, uIdx, storeVal);
                        ce.view.needsRememberedBarrier = false;
                    }
                }
                localExecuted += spanCost;
                ce.view.data[uIdx] = storeVal;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(StoreElementObjFast) {
                auto& ce = arrayCache[ip->src0Slot & 3u];
                const Value& arrVal = frameBase[ip->src0Slot];
                if (RS_FLAT_UNLIKELY(!ce.valid ||
                                     arrVal.rawTag() != ValueTag::ObjectRef ||
                                     arrVal.u_.objectRef != ce.ref)) {
                    if (!bindArrayCacheSlot(arrVal, ce)) goto L_FlatMathFallback;
                }
                const auto expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1));
                const auto expectedTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                if (RS_FLAT_UNLIKELY(ce.view.elementType != expectedType ||
                                     ce.view.elementTypeId != expectedTid)) {
                    goto L_FlatMathFallback;
                }
                const Value& storeVal = frameBase[ip->src2Slot];
                const ValueTag vtag = storeVal.rawTag();
                bool needsBarrier = false;
                bool needsPoolPin = false;
                if (expectedType == semantic::PrimitiveType::Object) {
                    if (vtag == ValueTag::NullObject) {
                        // Valid null reference; no GC barrier needed
                    } else if (vtag == ValueTag::ObjectRef) {
                        const semantic::SymbolId actualTid = state.heap->objectTypeIdFast(storeVal.u_.objectRef);
                        if (RS_FLAT_UNLIKELY(actualTid == 0)) goto L_FlatMathFallback;
                        if (actualTid != expectedTid && expectedTid != 0) {
                            if (actualTid != cachedAssignableSubTid || expectedTid != cachedAssignableBaseTid) {
                                if (RS_FLAT_UNLIKELY(!runtimeAssignable(state, actualTid, expectedTid))) {
                                    goto L_FlatMathFallback;
                                }
                                cachedAssignableSubTid = actualTid;
                                cachedAssignableBaseTid = expectedTid;
                            }
                        }
                        needsBarrier = true;
                    } else {
                        goto L_FlatMathFallback;
                    }
                } else {
                    // PrimitiveType::String
                    if (vtag == ValueTag::NullString) {
                        // Valid null string
                    } else if (vtag == ValueTag::String) {
                        needsPoolPin = true;
                    } else if (vtag == ValueTag::ObjectRef &&
                               storeVal.u_.objectRef.kind == ObjectKind::String &&
                               state.heap->isAlive(storeVal.u_.objectRef)) {
                        needsBarrier = true;
                    } else {
                        goto L_FlatMathFallback;
                    }
                }

                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                std::size_t uIdx = 0;
                if ((ip->arithFlags & detail::FlatInstruction::FlagArrayNoBoundsCheck) != 0u) {
                    uIdx = static_cast<std::size_t>(frameBase[ip->src1Slot].asInt64Unchecked());
                } else {
                    std::int64_t idx = 0;
                    if (RS_FLAT_UNLIKELY(!frameBase[ip->src1Slot].tryGetInt64(idx) ||
                                         static_cast<std::uint64_t>(idx) >= ce.view.length)) {
                        goto L_FlatMathFallback;
                    }
                    uIdx = static_cast<std::size_t>(idx);
                }
                if (needsPoolPin) {
                    pinValueInPools(storeVal);
                }
                if (needsBarrier) {
                    if (ce.view.dirtyCards != nullptr) {
                        ce.view.dirtyCards[uIdx >> ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (RS_FLAT_UNLIKELY(ce.view.needsRememberedBarrier || gcActive || state.heap->phase() != GcPhase::Idle)) {
                        state.heap->recordArrayWriteBarrier(ce.ref, uIdx, storeVal);
                        ce.view.needsRememberedBarrier = false;
                    }
                }
                localExecuted += spanCost;
                ce.view.data[uIdx] = storeVal;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(SuperLessLocalArrayLengthBranch) {
                const std::uint64_t cost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || cost > budgetLimit - localExecuted)) {
                    goto L_FlatSuperFallback;
                }
                std::int64_t left = 0;
                if (RS_FLAT_UNLIKELY(!frameBase[ip->src0Slot].tryGetInt64(left))) {
                    goto L_FlatSuperFallback;
                }
                auto& ce = arrayCache[ip->src1Slot & 3u];
                const Value& arrVal = frameBase[ip->src1Slot];
                std::int64_t right = 0;
                if (RS_FLAT_LIKELY((ce.valid &&
                                    arrVal.rawTag() == ValueTag::ObjectRef &&
                                    arrVal.u_.objectRef == ce.ref) ||
                                   bindArrayCacheSlot(arrVal, ce))) {
                    right = static_cast<std::int64_t>(ce.view.length);
                } else if (arrVal.rawTag() == ValueTag::ObjectRef && state.heap != nullptr) {
                    const auto lenOpt = state.heap->arrayLength(arrVal.u_.objectRef);
                    if (!lenOpt || *lenOpt > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                        goto L_FlatSuperFallback;
                    }
                    right = static_cast<std::int64_t>(*lenOpt);
                } else {
                    goto L_FlatSuperFallback;
                }
                bool cond = false;
                switch (ip->binaryOpcode1) {
                case detail::QuickOpcode::LessInt: cond = (left < right); break;
                case detail::QuickOpcode::LessOrEqualInt: cond = (left <= right); break;
                case detail::QuickOpcode::GreaterInt: cond = (left > right); break;
                case detail::QuickOpcode::GreaterOrEqualInt: cond = (left >= right); break;
                case detail::QuickOpcode::EqualInt: cond = (left == right); break;
                case detail::QuickOpcode::NotEqualInt: cond = (left != right); break;
                default: goto L_FlatSuperFallback;
                }
                localExecuted += cost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                ++state.statistics.branchesTaken;
                ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(NewObjectFast) {
                if (RS_FLAT_UNLIKELY(state.heap == nullptr)) goto L_FlatMathFallback;
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                ++localExecuted;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                else gcAccounted = localExecuted;

                const auto& tmpl = detail::executionMirror(*state.program).objectTemplates[ip->aux0Slot];
                bool slotsMoved = false;
                RuntimeError allocErr;
                const auto ref = state.heap->allocateObjectFromTemplate(
                    tmpl.typeId,
                    tmpl.defaultFields.data(),
                    tmpl.defaultFields.size(),
                    tmpl.referenceFields,
                    &allocErr,
                    &slotsMoved);
                if (RS_FLAT_LIKELY(ref.has_value())) {
                    if (RS_FLAT_UNLIKELY(slotsMoved)) {
                        invalidateAllFlatCaches();
                    }
                    if (gcBudgetEnabled) {
                        gcActive = (state.heap->phase() != GcPhase::Idle || state.heap->collectionRequested());
                    }
                    if (spanCost > 1u) {
                        localExecuted += (spanCost - 1u);
                    }
                    frameBase[ip->dstSlot] = *ref;
                    auto& oce = objectCache[ref->slot & 3u];
                    if (state.heap->tryBindObjectFastView(*ref, oce.view)) {
                        oce.ref = *ref;
                        oce.verifiedBaseTid = 0;
                        oce.valid = true;
                    }
                    RS_FLAT_DISPATCH_NEXT();
                }
                state.executed = localExecuted;
                fail(state,
                    allocErr.code == ErrorCode::None ? ErrorCode::OutOfMemory : allocErr.code,
                    allocErr.message.empty() ? "managed object allocation failed" : allocErr.message);
                goto L_FlatError;
            }

            RS_FLAT_CASE(LoadFieldExactFast) {
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool ownerOk = (oce.view.typeId == expectedOwnerTid) ||
                                       (oce.verifiedBaseTid == expectedOwnerTid);
                        if (RS_FLAT_UNLIKELY(!ownerOk)) {
                            if (runtimeAssignable(state, oce.view.typeId, expectedOwnerTid)) {
                                oce.verifiedBaseTid = expectedOwnerTid;
                                ownerOk = true;
                            }
                        }
                        const std::uint16_t fIdx = ip->aux0Slot;
                        if (RS_FLAT_LIKELY(ownerOk && fIdx < oce.view.fieldCount)) {
                            const Value& val = oce.view.fields[fIdx];
                            if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(val.rawTag()) ==
                                               static_cast<std::uint8_t>(ip->binaryOpcode1))) {
                                const std::uint64_t spanCost = ip->span;
                                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                                    if (spanCost > 1u) goto L_FlatSuperFallback;
                                    goto L_FlatBudgetExceeded;
                                }
                                localExecuted += spanCost;
                                frameBase[ip->dstSlot] = val;
                                RS_FLAT_DISPATCH_NEXT();
                            }
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(StoreFieldExactFast) {
                const Value& recvVal = frameBase[ip->src0Slot];
                if (RS_FLAT_LIKELY(recvVal.rawTag() == ValueTag::ObjectRef)) {
                    const ObjectRef ref = recvVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool ownerOk = (oce.view.typeId == expectedOwnerTid) ||
                                       (oce.verifiedBaseTid == expectedOwnerTid);
                        if (RS_FLAT_UNLIKELY(!ownerOk)) {
                            if (runtimeAssignable(state, oce.view.typeId, expectedOwnerTid)) {
                                oce.verifiedBaseTid = expectedOwnerTid;
                                ownerOk = true;
                            }
                        }
                        const std::uint16_t fIdx = ip->aux0Slot;
                        if (RS_FLAT_LIKELY(ownerOk && fIdx < oce.view.fieldCount)) {
                            const Value& rhs = frameBase[ip->src1Slot];
                            if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(rhs.rawTag()) ==
                                               static_cast<std::uint8_t>(ip->binaryOpcode1))) {
                                const std::uint64_t spanCost = ip->span;
                                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                                    if (spanCost > 1u) goto L_FlatSuperFallback;
                                    goto L_FlatBudgetExceeded;
                                }
                                localExecuted += spanCost;
                                oce.view.fields[fIdx] = rhs;
                                RS_FLAT_DISPATCH_NEXT();
                            }
                        }
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(IsTypeObjFast) {
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& srcVal = frameBase[ip->src0Slot];
                const ValueTag vtag = srcVal.rawTag();
                if (vtag == ValueTag::NullObject || vtag == ValueTag::Monostate) {
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot].storeBool(false);
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_LIKELY(vtag == ValueTag::ObjectRef)) {
                    const ObjectRef ref = srcVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto targetTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool matches = (oce.view.typeId == targetTid) ||
                                       (oce.verifiedBaseTid == targetTid) ||
                                       (cachedAssignableSubTid == oce.view.typeId &&
                                        cachedAssignableBaseTid == targetTid);
                        if (!matches) {
                            matches = runtimeAssignable(state, oce.view.typeId, targetTid);
                            if (matches) {
                                oce.verifiedBaseTid = targetTid;
                                cachedAssignableSubTid = oce.view.typeId;
                                cachedAssignableBaseTid = targetTid;
                            }
                        }
                        localExecuted += spanCost;
                        frameBase[ip->dstSlot].storeBool(matches);
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(AsTypeObjFast) {
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& srcVal = frameBase[ip->src0Slot];
                const ValueTag vtag = srcVal.rawTag();
                if (vtag == ValueTag::NullObject || vtag == ValueTag::Monostate) {
                    localExecuted += spanCost;
                    frameBase[ip->dstSlot] = NullObject{};
                    RS_FLAT_DISPATCH_NEXT();
                }
                if (RS_FLAT_LIKELY(vtag == ValueTag::ObjectRef)) {
                    const ObjectRef ref = srcVal.u_.objectRef;
                    auto& oce = objectCache[ref.slot & 3u];
                    if (RS_FLAT_LIKELY((oce.valid && oce.ref == ref) || bindObjectCacheEntry(ref, oce))) {
                        const auto targetTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                        bool matches = (oce.view.typeId == targetTid) ||
                                       (oce.verifiedBaseTid == targetTid) ||
                                       (cachedAssignableSubTid == oce.view.typeId &&
                                        cachedAssignableBaseTid == targetTid);
                        if (!matches) {
                            matches = runtimeAssignable(state, oce.view.typeId, targetTid);
                            if (matches) {
                                oce.verifiedBaseTid = targetTid;
                                cachedAssignableSubTid = oce.view.typeId;
                                cachedAssignableBaseTid = targetTid;
                            }
                        }
                        localExecuted += spanCost;
                        if (matches) {
                            frameBase[ip->dstSlot] = srcVal;
                        } else {
                            frameBase[ip->dstSlot] = NullObject{};
                        }
                        RS_FLAT_DISPATCH_NEXT();
                    }
                }
                goto L_FlatMathFallback;
            }

            RS_FLAT_CASE(NewDelegateFast) {
                if (RS_FLAT_UNLIKELY(state.heap == nullptr || state.mirror == nullptr ||
                                     ip->auxImm32 >= state.mirror->newDelegateSites.size())) {
                    goto L_FlatMathFallback;
                }
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const auto& site = state.mirror->newDelegateSites[ip->auxImm32];
                if (RS_FLAT_UNLIKELY(!site.valid)) goto L_FlatMathFallback;

                std::array<Value, 8> fields = site.templateFields;
                if (site.hasReceiverOperand) {
                    const Value& recvVal = frameBase[ip->src0Slot];
                    if (site.expectedReceiverType == semantic::PrimitiveType::Object) {
                        const ValueTag vt = recvVal.rawTag();
                        if (vt == ValueTag::NullObject) {
                            // Valid null receiver target
                        } else if (RS_FLAT_LIKELY(vt == ValueTag::ObjectRef)) {
                            const ObjectRef rRef = recvVal.u_.objectRef;
                            auto& rOce = objectCache[rRef.slot & 3u];
                            if (RS_FLAT_UNLIKELY(!(rOce.valid && rOce.ref == rRef) &&
                                                 !bindObjectCacheEntry(rRef, rOce))) {
                                goto L_FlatMathFallback;
                            }
                            const semantic::SymbolId expTid = site.expectedReceiverTypeId;
                            if (expTid != 0 &&
                                rOce.view.typeId != expTid &&
                                rOce.verifiedBaseTid != expTid &&
                                !(cachedAssignableSubTid == rOce.view.typeId &&
                                  cachedAssignableBaseTid == expTid)) {
                                if (RS_FLAT_UNLIKELY(!runtimeAssignable(state, rOce.view.typeId, expTid))) {
                                    goto L_FlatMathFallback;
                                }
                                rOce.verifiedBaseTid = expTid;
                                cachedAssignableSubTid = rOce.view.typeId;
                                cachedAssignableBaseTid = expTid;
                            }
                        } else {
                            goto L_FlatMathFallback;
                        }
                    } else {
                        goto L_FlatMathFallback;
                    }
                    fields[0] = recvVal;
                }

                localExecuted += spanCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                else gcAccounted = localExecuted;

                bool slotsMoved = false;
                RuntimeError allocErr;
                const auto ref = state.heap->allocateObjectFromTemplate(
                    site.delegateTypeId,
                    fields.data(),
                    fields.size(),
                    delegateReferenceFields(),
                    &allocErr,
                    &slotsMoved);
                if (RS_FLAT_LIKELY(ref.has_value())) {
                    if (RS_FLAT_UNLIKELY(slotsMoved)) {
                        invalidateAllFlatCaches();
                    }
                    if (gcBudgetEnabled) {
                        gcActive = (state.heap->phase() != GcPhase::Idle ||
                                    state.heap->collectionRequested());
                    }
                    frameBase[ip->dstSlot] = *ref;
                    auto& oce = objectCache[ref->slot & 3u];
                    if (state.heap->tryBindObjectFastView(*ref, oce.view)) {
                        oce.ref = *ref;
                        oce.verifiedBaseTid = 0;
                        oce.valid = true;
                    }
                    RS_FLAT_DISPATCH_NEXT();
                }
                state.executed = localExecuted;
                fail(state,
                    allocErr.code == ErrorCode::None ? ErrorCode::OutOfMemory : allocErr.code,
                    allocErr.message.empty() ? "delegate allocation failed" : allocErr.message);
                goto L_FlatError;
            }

            RS_FLAT_CASE(CombineDelegateFast) {
                if (RS_FLAT_UNLIKELY(state.heap == nullptr)) goto L_FlatMathFallback;
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& leftVal = frameBase[ip->src0Slot];
                const Value& rightVal = frameBase[ip->src1Slot];
                const auto delTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                const ValueTag lTag = leftVal.rawTag();
                const ValueTag rTag = rightVal.rawTag();
                if (RS_FLAT_UNLIKELY((lTag != ValueTag::NullObject && lTag != ValueTag::ObjectRef) ||
                                     (rTag != ValueTag::NullObject && rTag != ValueTag::ObjectRef))) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                else gcAccounted = localExecuted;
                state.executed = localExecuted;

                bool slotsMoved = false;
                Value combinedOut;
                if (RS_FLAT_UNLIKELY(!combineOrRemoveDelegateFastImpl(
                        state, delTid, leftVal, rightVal, false, combinedOut, &slotsMoved))) {
                    goto L_FlatError;
                }
                if (RS_FLAT_UNLIKELY(slotsMoved)) {
                    invalidateAllFlatCaches();
                }
                if (gcBudgetEnabled) {
                    gcActive = (state.heap->phase() != GcPhase::Idle ||
                                state.heap->collectionRequested());
                }
                frameBase[ip->dstSlot] = combinedOut;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(RemoveDelegateFast) {
                if (RS_FLAT_UNLIKELY(state.heap == nullptr)) goto L_FlatMathFallback;
                const std::uint64_t spanCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || spanCost > budgetLimit - localExecuted)) {
                    if (spanCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                const Value& leftVal = frameBase[ip->src0Slot];
                const Value& rightVal = frameBase[ip->src1Slot];
                const auto delTid = static_cast<semantic::SymbolId>(ip->integerImmediate);
                const ValueTag lTag = leftVal.rawTag();
                const ValueTag rTag = rightVal.rawTag();
                if (RS_FLAT_UNLIKELY((lTag != ValueTag::NullObject && lTag != ValueTag::ObjectRef) ||
                                     (rTag != ValueTag::NullObject && rTag != ValueTag::ObjectRef))) {
                    goto L_FlatMathFallback;
                }
                localExecuted += spanCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                else gcAccounted = localExecuted;
                state.executed = localExecuted;

                bool slotsMoved = false;
                Value removedOut;
                if (RS_FLAT_UNLIKELY(!combineOrRemoveDelegateFastImpl(
                        state, delTid, leftVal, rightVal, true, removedOut, &slotsMoved))) {
                    goto L_FlatError;
                }
                if (RS_FLAT_UNLIKELY(slotsMoved)) {
                    invalidateAllFlatCaches();
                }
                if (gcBudgetEnabled) {
                    gcActive = (state.heap->phase() != GcPhase::Idle ||
                                state.heap->collectionRequested());
                }
                frameBase[ip->dstSlot] = removedOut;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(InvokeDelegateFast) {
                const std::uint64_t baseCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || baseCost > budgetLimit - localExecuted)) {
                    if (baseCost > 1u) goto L_FlatSuperFallback;
                    goto L_FlatBudgetExceeded;
                }
                if (RS_FLAT_UNLIKELY(state.heap == nullptr || state.mirror == nullptr ||
                                     state.delegateCallCache == nullptr ||
                                     ip->auxImm32 >= state.mirror->invokeDelegateSites.size() ||
                                     ip->auxImm32 >= state.delegateCallCache->size())) {
                    goto L_FlatMathFallback;
                }
                const auto& siteDesc = state.mirror->invokeDelegateSites[ip->auxImm32];
                const Value& delVal = frameBase[ip->src0Slot];
                const ValueTag delTag = delVal.rawTag();
                if (RS_FLAT_UNLIKELY(delTag == ValueTag::NullObject || delTag == ValueTag::Monostate)) {
                    localExecuted += baseCost;
                    state.executed = localExecuted;
                    fail(state, ErrorCode::NullReference,
                        "delegate invocation attempted to dereference null");
                    goto L_FlatError;
                }
                if (RS_FLAT_UNLIKELY(delTag != ValueTag::ObjectRef)) goto L_FlatMathFallback;

                const ObjectRef delRef = delVal.u_.objectRef;
                auto& delOce = objectCache[delRef.slot & 3u];
                if (RS_FLAT_UNLIKELY(!(delOce.valid && delOce.ref == delRef) &&
                                     !bindObjectCacheEntry(delRef, delOce))) {
                    goto L_FlatMathFallback;
                }
                if (RS_FLAT_UNLIKELY(delOce.view.typeId != siteDesc.delegateTypeId ||
                                     delOce.view.fieldCount != 8u)) {
                    goto L_FlatMathFallback;
                }

                // Extract and type-validate user arguments into a C++ stack buffer (zero heap malloc!)
                const std::size_t uCount = siteDesc.userParamTypes.size();
                std::array<Value, 8> userArgsStack{};
                std::vector<Value> userArgsHeap;
                Value* userArgsPtr = userArgsStack.data();
                if (RS_FLAT_UNLIKELY(uCount > userArgsStack.size())) {
                    userArgsHeap.resize(uCount);
                    userArgsPtr = userArgsHeap.data();
                }
                if (ip->inlineArgCount >= 1u) {
                    if (uCount >= 1u) userArgsPtr[0] = frameBase[ip->src1Slot];
                    if (uCount >= 2u) userArgsPtr[1] = frameBase[ip->src2Slot];
                } else {
                    const auto& cold = flatColdBase[ip - streamBase];
                    const std::size_t regBase = execution->registerOffset;
                    for (std::size_t k = 0; k < uCount; ++k) {
                        userArgsPtr[k] = frameBase[regBase + cold.bytecode->operands[k + 1u]];
                    }
                }
                bool userArgsValid = true;
                for (std::size_t k = 0; k < uCount; ++k) {
                    const std::uint8_t expTag = siteDesc.userParamExactTags[k];
                    if (expTag != 0u) {
                        if (RS_FLAT_UNLIKELY(static_cast<std::uint8_t>(userArgsPtr[k].rawTag()) != expTag)) {
                            userArgsValid = false;
                            break;
                        }
                    } else {
                        const auto pt = siteDesc.userParamTypes[k];
                        const auto ptId = siteDesc.userParamTypeIds[k];
                        const ValueTag vt = userArgsPtr[k].rawTag();
                        if (pt == semantic::PrimitiveType::Object) {
                            if (vt == ValueTag::NullObject) {
                                // ok
                            } else if (vt == ValueTag::ObjectRef) {
                                const auto actTid = state.heap->objectTypeIdFast(userArgsPtr[k].u_.objectRef);
                                if (actTid == 0) {
                                    userArgsValid = false;
                                    break;
                                }
                                if (ptId != 0 && actTid != ptId &&
                                    !(cachedAssignableSubTid == actTid && cachedAssignableBaseTid == ptId)) {
                                    if (!runtimeAssignable(state, actTid, ptId)) {
                                        userArgsValid = false;
                                        break;
                                    }
                                    cachedAssignableSubTid = actTid;
                                    cachedAssignableBaseTid = ptId;
                                }
                            } else {
                                userArgsValid = false;
                                break;
                            }
                        } else {
                            // Complex Struct / Enum / String / Array: verify cleanly via expectSignatureType-free check
                            userArgsValid = false;
                            break;
                        }
                    }
                }
                if (RS_FLAT_UNLIKELY(!userArgsValid)) goto L_FlatMathFallback;

                std::int64_t rootFlags = -1;
                if (RS_FLAT_UNLIKELY(!delOce.view.fields[3].tryGetInt64(rootFlags) ||
                                     rootFlags < 0 || rootFlags > 4)) {
                    goto L_FlatMathFallback;
                }

                auto& siteCache = (*state.delegateCallCache)[ip->auxImm32];

                const auto tryEvalTrivialLeaf = [&](const ResolvedDelegateLeaf& leaf,
                                                    std::uint64_t preCallCost,
                                                    Value& outTrivialVal) -> bool {
                    const auto* calleeExec = leaf.execution;
                    const auto tKind = calleeExec->trivialKind;
                    if (tKind == detail::TrivialMethodKind::None ||
                        RS_FLAT_UNLIKELY(state.stack.size() >= state.limits.recursionLimit)) {
                        return false;
                    }
                    const std::uint64_t totalCost = preCallCost + calleeExec->trivialInstCost;
                    if (RS_FLAT_UNLIKELY(totalCost > budgetLimit - localExecuted)) {
                        return false;
                    }
                    const std::size_t bias = leaf.hasBoundReceiver ? 1u : 0u;
                    bool handled = false;
                    switch (tKind) {
                    case detail::TrivialMethodKind::ReturnVoid:
                        handled = true;
                        break;
                    case detail::TrivialMethodKind::ReturnConst:
                        outTrivialVal = calleeExec->trivialConstValue;
                        handled = true;
                        break;
                    case detail::TrivialMethodKind::ReturnParam: {
                        const std::uint16_t pIdx = calleeExec->trivialParamIndex;
                        if (leaf.hasBoundReceiver && pIdx == 0) {
                            outTrivialVal = leaf.receiverVal;
                            handled = true;
                        } else if (pIdx >= bias && (pIdx - bias) < uCount) {
                            outTrivialVal = userArgsPtr[pIdx - bias];
                            handled = true;
                        }
                        break;
                    }
                    case detail::TrivialMethodKind::ReturnThisFieldExact:
                    case detail::TrivialMethodKind::StoreThisFieldExact:
                    case detail::TrivialMethodKind::ReturnThisFieldAddField:
                    case detail::TrivialMethodKind::ReturnThisFieldSubField:
                    case detail::TrivialMethodKind::ReturnThisFieldAddConst:
                    case detail::TrivialMethodKind::ReturnThisFieldSubConst:
                    case detail::TrivialMethodKind::ReturnThisFieldMulConst: {
                        if (!leaf.hasBoundReceiver || leaf.receiverVal.rawTag() != ValueTag::ObjectRef) {
                            break;
                        }
                        auto& rOce = objectCache[leaf.receiverRef.slot & 3u];
                        if (RS_FLAT_UNLIKELY(!(rOce.valid && rOce.ref == leaf.receiverRef) &&
                                             !bindObjectCacheEntry(leaf.receiverRef, rOce))) {
                            break;
                        }
                        const std::uint16_t f0 = calleeExec->trivialFieldIndex0;
                        if (tKind == detail::TrivialMethodKind::ReturnThisFieldExact) {
                            if (RS_FLAT_LIKELY(f0 < rOce.view.fieldCount &&
                                               static_cast<std::uint8_t>(rOce.view.fields[f0].rawTag()) ==
                                                   calleeExec->trivialFieldTag0)) {
                                outTrivialVal = rOce.view.fields[f0];
                                handled = true;
                            }
                        } else if (tKind == detail::TrivialMethodKind::StoreThisFieldExact) {
                            const std::uint16_t pIdx = calleeExec->trivialParamIndex;
                            if (pIdx >= bias && (pIdx - bias) < uCount &&
                                RS_FLAT_LIKELY(f0 < rOce.view.fieldCount)) {
                                const Value& rhs = userArgsPtr[pIdx - bias];
                                if (RS_FLAT_LIKELY(static_cast<std::uint8_t>(rhs.rawTag()) ==
                                                   calleeExec->trivialFieldTag0)) {
                                    rOce.view.fields[f0] = rhs;
                                    handled = true;
                                }
                            }
                        } else if (tKind == detail::TrivialMethodKind::ReturnThisFieldAddField ||
                                   tKind == detail::TrivialMethodKind::ReturnThisFieldSubField) {
                            const std::uint16_t f1 = calleeExec->trivialFieldIndex1;
                            std::int64_t a = 0, b = 0;
                            if (RS_FLAT_LIKELY(f0 < rOce.view.fieldCount &&
                                               f1 < rOce.view.fieldCount &&
                                               rOce.view.fields[f0].tryGetInt64(a) &&
                                               rOce.view.fields[f1].tryGetInt64(b))) {
                                const std::int64_t r =
                                    (tKind == detail::TrivialMethodKind::ReturnThisFieldAddField)
                                        ? (a + b)
                                        : (a - b);
                                if (!calleeExec->trivialCheckedArith) {
                                    outTrivialVal.storeInt64(wrapInt32(r));
                                    handled = true;
                                } else if (RS_FLAT_LIKELY(isValidInt32Payload(r))) {
                                    outTrivialVal.storeInt64(r);
                                    handled = true;
                                }
                            }
                        } else {
                            std::int64_t a = 0;
                            if (RS_FLAT_LIKELY(f0 < rOce.view.fieldCount &&
                                               rOce.view.fields[f0].tryGetInt64(a))) {
                                const std::int64_t c = calleeExec->trivialConstValue.asInt64Unchecked();
                                std::int64_t r = 0;
                                bool ovf = false;
                                if (tKind == detail::TrivialMethodKind::ReturnThisFieldAddConst) r = a + c;
                                else if (tKind == detail::TrivialMethodKind::ReturnThisFieldSubConst) r = a - c;
                                else {
#if defined(__GNUC__) || defined(__clang__)
                                    ovf = __builtin_mul_overflow(a, c, &r);
#else
                                    if (RS_FLAT_LIKELY(isValidInt32Payload(a) && isValidInt32Payload(c))) {
                                        r = a * c;
                                    } else {
                                        ovf = true;
                                    }
#endif
                                }
                                if (!ovf) {
                                    if (!calleeExec->trivialCheckedArith) {
                                        outTrivialVal.storeInt64(wrapInt32(r));
                                        handled = true;
                                    } else if (RS_FLAT_LIKELY(isValidInt32Payload(r))) {
                                        outTrivialVal.storeInt64(r);
                                        handled = true;
                                    }
                                }
                            }
                        }
                        break;
                    }
                    case detail::TrivialMethodKind::None:
                        break;
                    }
                    if (RS_FLAT_LIKELY(handled)) {
                        localExecuted += totalCost;
                        ++state.statistics.functionCalls;
                        if (state.stack.size() + 1 > state.statistics.maximumCallDepth) {
                            state.statistics.maximumCallDepth = state.stack.size() + 1;
                        }
                        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                        return true;
                    }
                    return false;
                };

                if (RS_FLAT_LIKELY(rootFlags != 4)) {
                    // Single-cast 16-entry direct-mapped IC keyed by immutable delegate ObjectRef
                    auto& slotWay = siteCache.singleWays[delRef.slot & 15u];
                    const ResolvedDelegateLeaf* hit = nullptr;
                    if (RS_FLAT_LIKELY(
                            slotWay.valid &&
                            slotWay.leafRef == delRef &&
                            (!slotWay.hasBoundReceiver ||
                             slotWay.receiverVal.rawTag() != ValueTag::ObjectRef ||
                             state.heap->isAlive(slotWay.receiverRef)))) {
                        hit = &slotWay;
                    } else {
                        ResolvedDelegateLeaf resolved{};
                        if (!resolveDelegateLeafFast(
                                state, *state.mirror, delRef, delOce.view, uCount, siteCache, resolved)) {
                            goto L_FlatMathFallback;
                        }
                        slotWay = std::move(resolved);
                        hit = &slotWay;
                    }

                    Value callResult{};
                    if (tryEvalTrivialLeaf(*hit, baseCost, callResult)) {
                        if (ip->dstSlot != detail::InvalidCompactReg) {
                            frameBase[ip->dstSlot] = callResult;
                        }
                        RS_FLAT_DISPATCH_NEXT();
                    }

                    localExecuted += baseCost;
                    if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                    state.executed = localExecuted;

                    const ResolvedDelegateLeaf hitLeaf = *hit;
                    bool ok = false;
                    if (!hitLeaf.hasBoundReceiver) {
                        ok = executeFunction(
                            state,
                            *hitLeaf.target,
                            ArgumentSource(userArgsPtr, uCount),
                            callResult,
                            hitLeaf.execution,
                            true);
                    } else if (RS_FLAT_LIKELY(uCount + 1u <= 8u)) {
                        std::array<Value, 8> boundArgs{};
                        boundArgs[0] = hitLeaf.receiverVal;
                        for (std::size_t k = 0; k < uCount; ++k) boundArgs[k + 1u] = userArgsPtr[k];
                        ok = executeFunction(
                            state,
                            *hitLeaf.target,
                            ArgumentSource(boundArgs.data(), uCount + 1u),
                            callResult,
                            hitLeaf.execution,
                            true);
                    } else {
                        std::vector<Value> boundVec;
                        boundVec.reserve(uCount + 1u);
                        boundVec.push_back(hitLeaf.receiverVal);
                        for (std::size_t k = 0; k < uCount; ++k) boundVec.push_back(userArgsPtr[k]);
                        ok = executeFunction(
                            state,
                            *hitLeaf.target,
                            ArgumentSource(boundVec),
                            callResult,
                            hitLeaf.execution,
                            true);
                    }

                    frameBase = state.values.data() + frameOffset;
                    if (RS_FLAT_LIKELY(hitLeaf.execution->isSmallLeafInlineCandidate && !gcBudgetEnabled)) {
                        localExecuted = state.executed;
                        gcAccounted = localExecuted;
                    } else {
                        syncAfterFlatSubcall();
                    }
                    if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                    if (ip->dstSlot != detail::InvalidCompactReg) {
                        frameBase[ip->dstSlot] = std::move(callResult);
                    }
                    RS_FLAT_DISPATCH_NEXT();
                }

                // Multicast fast path (flags == 4): pre-flattened & pre-resolved leaf vector cache!
                bool mcCacheHit = (siteCache.multicastValid && siteCache.multicastRef == delRef);
                if (mcCacheHit) {
                    for (const auto& leaf : siteCache.multicastLeaves) {
                        if (!state.heap->isAlive(leaf.leafRef) ||
                            (leaf.hasBoundReceiver &&
                             leaf.receiverVal.rawTag() == ValueTag::ObjectRef &&
                             !state.heap->isAlive(leaf.receiverRef))) {
                            mcCacheHit = false;
                            break;
                        }
                    }
                }
                std::vector<ResolvedDelegateLeaf> fallbackMulticastLeaves;
                std::vector<ResolvedDelegateLeaf>* activeLeaves = &siteCache.multicastLeaves;
                bool pinnedSiteCache = false;
                if (RS_FLAT_LIKELY(mcCacheHit)) {
                    ++siteCache.multicastPinCount;
                    pinnedSiteCache = true;
                } else if (siteCache.multicastPinCount == 0u) {
                    siteCache.multicastValid = false;
                    if (!flattenAndResolveMulticastFast(
                            state, *state.mirror, siteDesc.delegateTypeId, delRef, uCount, siteCache, siteCache.multicastLeaves)) {
                        goto L_FlatMathFallback;
                    }
                    siteCache.multicastRef = delRef;
                    siteCache.multicastValid = true;
                    ++siteCache.multicastPinCount;
                    pinnedSiteCache = true;
                } else {
                    activeLeaves = &fallbackMulticastLeaves;
                    if (!flattenAndResolveMulticastFast(
                            state, *state.mirror, siteDesc.delegateTypeId, delRef, uCount, siteCache, fallbackMulticastLeaves)) {
                        goto L_FlatMathFallback;
                    }
                }
                struct MulticastPinGuard {
                    DelegateCallSiteCache& cache;
                    bool pinned = false;
                    ~MulticastPinGuard() {
                        if (pinned && cache.multicastPinCount > 0u) {
                            --cache.multicastPinCount;
                        }
                    }
                } pinGuard{siteCache, pinnedSiteCache};

                localExecuted += baseCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;

                Value lastResult{};
                std::array<Value, 8> mcBoundArgs{};
                const std::size_t leafCount = activeLeaves->size();
                for (std::size_t li = 0; li < leafCount; ++li) {
                    const ResolvedDelegateLeaf leaf = (*activeLeaves)[li];
                    if (tryEvalTrivialLeaf(leaf, 1u, lastResult)) {
                        state.executed = localExecuted;
                        continue;
                    }
                    if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) {
                        state.executed = localExecuted - 1u;
                        fail(state, ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                        goto L_FlatError;
                    }
                    if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                    state.executed = localExecuted;

                    bool ok = false;
                    if (!leaf.hasBoundReceiver) {
                        ok = executeFunction(
                            state,
                            *leaf.target,
                            ArgumentSource(userArgsPtr, uCount),
                            lastResult,
                            leaf.execution,
                            true);
                    } else if (RS_FLAT_LIKELY(uCount + 1u <= mcBoundArgs.size())) {
                        mcBoundArgs[0] = leaf.receiverVal;
                        for (std::size_t k = 0; k < uCount; ++k) mcBoundArgs[k + 1u] = userArgsPtr[k];
                        ok = executeFunction(
                            state,
                            *leaf.target,
                            ArgumentSource(mcBoundArgs.data(), uCount + 1u),
                            lastResult,
                            leaf.execution,
                            true);
                    } else {
                        std::vector<Value> boundVec;
                        boundVec.reserve(uCount + 1u);
                        boundVec.push_back(leaf.receiverVal);
                        for (std::size_t k = 0; k < uCount; ++k) boundVec.push_back(userArgsPtr[k]);
                        ok = executeFunction(
                            state,
                            *leaf.target,
                            ArgumentSource(boundVec),
                            lastResult,
                            leaf.execution,
                            true);
                    }
                    if (RS_FLAT_LIKELY(leaf.execution->isSmallLeafInlineCandidate && !gcBudgetEnabled)) {
                        localExecuted = state.executed;
                        gcAccounted = localExecuted;
                    } else {
                        syncAfterFlatSubcall();
                    }
                    if (RS_FLAT_UNLIKELY(!ok)) {
                        frameBase = state.values.data() + frameOffset;
                        goto L_FlatError;
                    }
                }
                frameBase = state.values.data() + frameOffset;
                if (ip->dstSlot != detail::InvalidCompactReg) {
                    frameBase[ip->dstSlot] = std::move(lastResult);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AsyncEnter) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AwaitSuspend) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const Value awaitableVal = frameBase[ip->src0Slot];
                frameBase[ip->storeSlot] = awaitableVal;
                AwaitableHandle awaitedHandle{};
                if (RS_FLAT_UNLIKELY(!state.asyncRuntime ||
                                     !tryExtractAwaitableHandle(awaitableVal, awaitedHandle) ||
                                     !state.asyncRuntime->awaitables().isValid(awaitedHandle))) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::InvalidProgram, "awaited value does not hold a valid Awaitable handle");
                    goto L_FlatError;
                }
                if (state.asyncRuntime->awaitables().isCompleted(awaitedHandle)) {
                    RS_FLAT_DISPATCH_NEXT();
                }
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!suspendInterpreterCoroutine(
                        state,
                        function,
                        activeCoroutine,
                        frameBase + execution->paramOffset,
                        argCount,
                        frameBase + execution->localOffset,
                        function.localTypes.size(),
                        frameBase + execution->registerOffset,
                        function.registerTypes.size(),
                        static_cast<std::uint32_t>(ip->integerImmediate),
                        awaitedHandle,
                        result))) {
                    goto L_FlatError;
                }
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return true;
            }

            RS_FLAT_CASE(AwaitResume) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const Value awaitableVal = frameBase[ip->src0Slot];
                frameBase[ip->src0Slot] = Value{};
                Value resumedVal{};
                const bool hasRes = (ip->dstSlot != detail::InvalidCompactReg);
                if (RS_FLAT_UNLIKELY(!resumeInterpreterAwaitable(
                        state,
                        awaitableVal,
                        hasRes,
                        static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(ip->binaryOpcode1)),
                        static_cast<semantic::SymbolId>(ip->integerImmediate),
                        resumedVal))) {
                    frameBase = state.values.data() + frameOffset;
                    syncAfterFlatSubcall();
                    goto L_FlatError;
                }
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (hasRes) {
                    frameBase[ip->dstSlot] = std::move(resumedVal);
                }
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(AsyncResolve) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const Value* payloadPtr =
                    (ip->inlineArgCount != 0u && ip->src0Slot != detail::InvalidCompactReg)
                        ? &frameBase[ip->src0Slot]
                        : nullptr;
                Value wrapped{};
                if (RS_FLAT_UNLIKELY(!resolveInterpreterCoroutine(
                        state,
                        activeCoroutine,
                        static_cast<semantic::SymbolId>(ip->integerImmediate),
                        payloadPtr,
                        wrapped))) {
                    goto L_FlatError;
                }
                frameBase[ip->dstSlot] = std::move(wrapped);
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(GenericBytecode) {
            L_FlatMathFallback:
                if (RS_FLAT_UNLIKELY(ip->span > 1)) goto L_FlatSuperFallback;
            L_FlatGenericFallback:
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                if (RS_FLAT_UNLIKELY(cold.inlinedCallee != nullptr)) {
                    const auto& cLoc = *cold.inlinedCallee;
                    const auto& cFunc = *cLoc.function;
                    const auto& cExec = *cold.directExecution;
                    const std::size_t subOff = frameOffset + cold.inlineFrameBias;
                    const ValueWindow argWin{&state.values, subOff + cExec.paramOffset, cFunc.parameterTypes.size()};
                    const ValueWindow locWin{&state.values, subOff + cExec.localOffset, cFunc.localTypes.size()};
                    const ValueWindow regWin{&state.values, subOff + cExec.registerOffset, cFunc.registerTypes.size()};
                    state.stack.push_back(&cLoc.qualifiedName);
                    const bool ok = executeInstruction(
                        state, *cLoc.module, cFunc, *cold.bytecode, argWin, locWin, regWin);
                    frameBase = state.values.data() + frameOffset;
                    syncAfterFlatSubcall();
                    if (RS_FLAT_UNLIKELY(!ok)) {
                        if (!state.pendingException) {
                            refineInterpreterFrameSourceLocation(
                                state,
                                cLoc,
                                cExec.blocks[cold.blockIndex].bytecode->id,
                                cold.instructionIndexInBlock,
                                ip->span,
                                false);
                        }
                        state.stack.pop_back();
                        if (!state.pendingException) {
                            refineInterpreterFrameSourceLocation(
                                state,
                                location,
                                execution->blocks[cold.callerBlockIndex].bytecode->id,
                                cold.callerInstIndex,
                                1,
                                false);
                        }
                        state.valueTop = frameOffset;
                        state.stack.pop_back();
                        return false;
                    }
                    state.stack.pop_back();
                    RS_FLAT_DISPATCH_NEXT();
                }
                const ValueWindow argWin{&state.values, frameOffset + execution->paramOffset, argCount};
                const ValueWindow locWin{&state.values, frameOffset + execution->localOffset, function.localTypes.size()};
                const ValueWindow regWin{&state.values, frameOffset + execution->registerOffset, function.registerTypes.size()};
                const bool ok = executeInstruction(
                    state, *location.module, function, *cold.bytecode, argWin, locWin, regWin);
                frameBase = state.values.data() + frameOffset;
                syncAfterFlatSubcall();
                if (RS_FLAT_UNLIKELY(!ok)) goto L_FlatError;
                RS_FLAT_DISPATCH_NEXT();
            }

            RS_FLAT_CASE(TermJumpNoArgs) {
                const std::uint64_t hopCost = ip->span;
                if (RS_FLAT_UNLIKELY(localExecuted > budgetLimit || hopCost > budgetLimit - localExecuted)) {
                    if (hopCost > 1u) {
                        // Step first hop if 1 budget unit remains
                        if (localExecuted < budgetLimit) {
                            ++localExecuted;
                            ++state.statistics.branchesTaken;
                            ip = streamBase + ip->auxImm32;
                        }
                    }
                    goto L_FlatBudgetExceeded;
                }
                localExecuted += hopCost;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.statistics.branchesTaken += hopCost;
                ip = streamBase + ip->trueTargetOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(TermBranchBoolNoArgs) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const bool cond = frameBase[ip->src0Slot].asBoolUnchecked();
                ++state.statistics.branchesTaken;
                ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(TermBranchGeneric) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const auto& cold = flatColdBase[ip - streamBase];
                const auto& blk = execution->blocks[cold.blockIndex];
                const auto& terminator = blk.bytecode->terminator;
                const detail::ExecutionEdge* edge = &blk.branch;
                std::uint32_t nextStreamOffset = ip->trueTargetOffset;
                if (terminator.kind == bytecode::TerminatorKind::Branch) {
                    const std::size_t condReg = terminator.condition;
                    if (RS_FLAT_UNLIKELY(condReg >= function.registerTypes.size() ||
                        !expectType(state, frameBase[execution->registerOffset + condReg],
                            semantic::PrimitiveType::Bool, "branch"))) {
                        state.executed = localExecuted;
                        goto L_FlatError;
                    }
                    if (!frameBase[execution->registerOffset + condReg].asBoolUnchecked()) {
                        edge = &blk.falseBranch;
                        nextStreamOffset = ip->falseTargetOffset;
                    }
                } else if (RS_FLAT_UNLIKELY(terminator.kind != bytecode::TerminatorKind::Jump)) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::InvalidProgram, "block has invalid terminator");
                    goto L_FlatError;
                }
                ++state.statistics.branchesTaken;
                if (RS_FLAT_UNLIKELY(edge->target == detail::InvalidBlockIndex ||
                    !edge->sources || !edge->destinations ||
                    edge->sources->size() != edge->destinations->size())) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::InvalidProgram, "branch target or argument count is invalid");
                    goto L_FlatError;
                }
                if (edge->hasArguments) {
                    const auto& sources = *edge->sources;
                    const auto& destinations = *edge->destinations;
                    if (RS_FLAT_UNLIKELY(sources.size() > execution->edgeScratchCount)) {
                        state.executed = localExecuted;
                        fail(state, ErrorCode::InvalidProgram, "branch scratch space is too small");
                        goto L_FlatError;
                    }
                    Value* const regPtr = frameBase + execution->registerOffset;
                    Value* const scratchPtr = frameBase + execution->scratchOffset;
                    const std::size_t regLimit = function.registerTypes.size();
                    for (std::size_t idx = 0; idx < sources.size(); ++idx) {
                        if (RS_FLAT_UNLIKELY(sources[idx] >= regLimit || destinations[idx].target >= regLimit)) {
                            state.executed = localExecuted;
                            fail(state, ErrorCode::InvalidProgram, "branch argument register is invalid");
                            goto L_FlatError;
                        }
                        scratchPtr[idx] = regPtr[sources[idx]];
                    }
                    for (std::size_t idx = 0; idx < sources.size(); ++idx) {
                        regPtr[destinations[idx].target] = std::move(scratchPtr[idx]);
                    }
                }
                ip = streamBase + nextStreamOffset;
                RS_FLAT_DISPATCH_AT_IP();
            }

            RS_FLAT_CASE(TermReturnVoid) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                result = std::monostate{};
                state.executed = localExecuted;
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return true;
            }

            RS_FLAT_CASE(TermReturnScalarFast) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                result = frameBase[ip->src0Slot];
                state.executed = localExecuted;
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return true;
            }

            RS_FLAT_CASE(TermReturnGeneric) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                const auto& cold = flatColdBase[ip - streamBase];
                const auto& terminator = execution->blocks[cold.blockIndex].bytecode->terminator;
                if (RS_FLAT_UNLIKELY(terminator.value >= function.registerTypes.size())) {
                    state.executed = localExecuted;
                    fail(state, ErrorCode::InvalidProgram, "return register is invalid");
                    goto L_FlatError;
                }
                const Value& retVal = frameBase[execution->registerOffset + terminator.value];
                state.executed = localExecuted;
                if (RS_FLAT_UNLIKELY(!expectSignatureType(state, retVal, function.returnType, function.returnTypeId, "return value"))) {
                    goto L_FlatError;
                }
                result = retVal;
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return true;
            }

            RS_FLAT_CASE(TermThrow) {
                if (RS_FLAT_UNLIKELY(++localExecuted > budgetLimit)) goto L_FlatBudgetExceeded;
                if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
                state.executed = localExecuted;
                const auto& cold = flatColdBase[ip - streamBase];
                const auto& terminator = execution->blocks[cold.blockIndex].bytecode->terminator;
                if (RS_FLAT_UNLIKELY(terminator.value >= function.registerTypes.size() ||
                    !std::holds_alternative<ObjectRef>(frameBase[execution->registerOffset + terminator.value]))) {
                    fail(state, ErrorCode::NullReference, "throw requires a non-null script object");
                    goto L_FlatError;
                }
                state.pendingException = frameBase[execution->registerOffset + terminator.value];
                goto L_FlatError;
            }

#if !(defined(__GNUC__) || defined(__clang__))
            default:
                goto L_FlatError;
            }
#endif

L_FlatSuperFallback: {
        // When instruction budget is exhausted mid-superinstruction (or overflow occurs in a folded op),
        // step the underlying single instructions one by one for exact instruction/location parity.
        if ((ip->arithFlags & detail::FlatInstruction::FlagPreJump1) != 0u) {
            if (localExecuted >= budgetLimit) {
                goto L_FlatBudgetExceeded;
            }
            ++localExecuted;
            ++state.statistics.branchesTaken;
            ip = streamBase + ip->auxImm32;
        }
        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
        state.executed = localExecuted;
        const auto& cold = flatColdBase[ip - streamBase];
        const bool isInlined = (cold.inlinedCallee != nullptr);
        const ProgramImage::FunctionLocation& activeLoc = isInlined ? *cold.inlinedCallee : location;
        const bytecode::Function& activeFunc = *activeLoc.function;
        const detail::ExecutionFunction& activeExec = isInlined ? *cold.directExecution : *execution;
        const std::size_t activeFrameOff = isInlined ? (frameOffset + cold.inlineFrameBias) : frameOffset;
        const std::size_t activeArgCount = isInlined ? activeFunc.parameterTypes.size() : argCount;
        const auto& blk = activeExec.blocks[cold.blockIndex];
        const ValueWindow argWin{&state.values, activeFrameOff + activeExec.paramOffset, activeArgCount};
        const ValueWindow locWin{&state.values, activeFrameOff + activeExec.localOffset, activeFunc.localTypes.size()};
        const ValueWindow regWin{&state.values, activeFrameOff + activeExec.registerOffset, activeFunc.registerTypes.size()};
        const std::size_t startIdx = cold.instructionIndexInBlock;
        const bool includesBranch =
            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalRegBranch ||
            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalConstBranch ||
            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalArrayLengthBranch;
        const bool includesJump =
            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump;
        const std::size_t instSpan = (includesBranch || includesJump)
            ? static_cast<std::size_t>(ip->span - 1)
            : static_cast<std::size_t>(ip->span);
        if (isInlined) {
            state.stack.push_back(&activeLoc.qualifiedName);
        }
        std::size_t executedSubK = 0;
        for (std::size_t k = 0; k < instSpan; ++k) {
            executedSubK = k;
            const auto& singleDec = blk.instructions[startIdx + k];
            const bool ok = (singleDec.opcode == detail::QuickOpcode::Generic ||
                             singleDec.opcode >= detail::QuickOpcode::ConstantDouble)
                ? executeInstruction(state, *activeLoc.module, activeFunc, *singleDec.bytecode, argWin, locWin, regWin)
                : executeQuickInstruction(state, *activeLoc.module, singleDec, argWin, locWin, regWin);
            frameBase = state.values.data() + frameOffset;
            if (!ok) {
                syncAfterFlatSubcall();
                if (isInlined) {
                    if (!state.pendingException) {
                        refineInterpreterFrameSourceLocation(
                            state,
                            activeLoc,
                            blk.bytecode->id,
                            static_cast<std::uint32_t>(startIdx + executedSubK),
                            1,
                            false);
                    }
                    state.stack.pop_back();
                    if (!state.pendingException) {
                        refineInterpreterFrameSourceLocation(
                            state,
                            location,
                            execution->blocks[cold.callerBlockIndex].bytecode->id,
                            cold.callerInstIndex,
                            1,
                            false);
                    }
                    if (state.pendingException && execution->hasExceptionHandlers) {
                        const auto protectedBlockId = execution->blocks[cold.callerBlockIndex].bytecode->id;
                        const auto handlerOffset = routeFastException(protectedBlockId);
                        if (handlerOffset != detail::InvalidStreamOffset) {
                            ip = streamBase + handlerOffset;
                            RS_FLAT_DISPATCH_AT_IP();
                        }
                    }
                    state.valueTop = frameOffset;
                    state.stack.pop_back();
                    return false;
                }
                if (!state.pendingException) {
                    refineInterpreterFrameSourceLocation(
                        state,
                        location,
                        blk.bytecode->id,
                        static_cast<std::uint32_t>(startIdx + executedSubK),
                        1,
                        false);
                    state.valueTop = frameOffset;
                    state.stack.pop_back();
                    return false;
                }
                goto L_FlatError;
            }
        }
        if (isInlined) {
            state.stack.pop_back();
        }
        syncAfterFlatSubcall();
        if (includesBranch) {
            if (++localExecuted > budgetLimit) goto L_FlatBudgetExceeded;
            if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
            const bool cond = frameBase[activeFrameOff - frameOffset + activeExec.registerOffset + blk.terminatorReg].asBoolUnchecked();
            ++state.statistics.branchesTaken;
            ip = streamBase + (cond ? ip->trueTargetOffset : ip->falseTargetOffset);
            RS_FLAT_DISPATCH_AT_IP();
        }
        if (includesJump) {
            if (++localExecuted > budgetLimit) goto L_FlatBudgetExceeded;
            if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
            ++state.statistics.branchesTaken;
            ip = streamBase + ip->trueTargetOffset;
            RS_FLAT_DISPATCH_AT_IP();
        }
        RS_FLAT_DISPATCH_NEXT();
    }

L_FlatBudgetExceeded:
        localExecuted = budgetLimit;
        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
        state.executed = budgetLimit;
        fail(state, ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        goto L_FlatError;

L_FlatTypeMismatch:
        state.executed = localExecuted;
        fail(state, ErrorCode::TypeMismatch, "expected int operand in flat quick instruction");
        goto L_FlatError;

L_FlatError:
        if (RS_FLAT_UNLIKELY(gcActive)) flushFlatGc();
        if (ip >= streamBase && ip < streamBase + execution->flatStream.size()) {
            const auto& cold = flatColdBase[ip - streamBase];
            if (cold.inlinedCallee != nullptr) {
                if (state.pendingException && execution->hasExceptionHandlers) {
                    const auto protectedBlockId = execution->blocks[cold.callerBlockIndex].bytecode->id;
                    const auto handlerOffset = routeFastException(protectedBlockId);
                    if (handlerOffset != detail::InvalidStreamOffset) {
                        ip = streamBase + handlerOffset;
                        RS_FLAT_DISPATCH_AT_IP();
                    }
                }
                if (!state.pendingException) {
                    if (cold.callerBlockIndex < execution->blocks.size()) {
                        refineInterpreterFrameSourceLocation(
                            state,
                            location,
                            execution->blocks[cold.callerBlockIndex].bytecode->id,
                            cold.callerInstIndex,
                            1,
                            false);
                    }
                    const auto& cLoc = *cold.inlinedCallee;
                    const auto& cExec = *cold.directExecution;
                    state.stack.push_back(&cLoc.qualifiedName);
                    state.error.stackTrace.insert(state.error.stackTrace.begin(), cLoc.qualifiedName);
                    StackTraceLocation calleeTraceLoc;
                    calleeTraceLoc.function = cLoc.qualifiedName;
                    if (cLoc.function) {
                        calleeTraceLoc.symbolId = cLoc.function->symbolId;
                        const auto& dbg = cLoc.function->debugInfo;
                        if (!dbg.sequencePoints.empty() && cLoc.module) {
                            const auto* activeSp = &dbg.sequencePoints.front();
                            for (const auto& src : cLoc.module->sourceFiles) {
                                if (src.id == activeSp->range.fileId) {
                                    calleeTraceLoc.sourcePath = src.path;
                                    break;
                                }
                            }
                            calleeTraceLoc.sourceLine =
                                static_cast<std::uint32_t>(activeSp->range.start.line + 1);
                            calleeTraceLoc.sourceColumn =
                                static_cast<std::uint32_t>(activeSp->range.start.column + 1);
                        }
                    }
                    state.error.sourceLocations.insert(state.error.sourceLocations.begin(), calleeTraceLoc);
                    state.error.symbolizedStackTrace.insert(
                        state.error.symbolizedStackTrace.begin(), calleeTraceLoc.format());
                    if (cold.blockIndex < cExec.blocks.size()) {
                        const bool isTerm =
                            ip->flatOpcode >= detail::FlatOpcode::TermJumpNoArgs ||
                            (ip->flatOpcode >= detail::FlatOpcode::InlineReturnVoid &&
                             ip->flatOpcode <= detail::FlatOpcode::InlineReturnScalarJump) ||
                            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalRegBranch ||
                            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalConstBranch ||
                            ip->flatOpcode == detail::FlatOpcode::SuperLessLocalArrayLengthBranch ||
                            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
                            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
                            ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump;
                        if (!isTerm) {
                            refineInterpreterFrameSourceLocation(
                                state,
                                cLoc,
                                cExec.blocks[cold.blockIndex].bytecode->id,
                                cold.instructionIndexInBlock,
                                ip->span,
                                false);
                        }
                    }
                    state.stack.pop_back();
                }
                if (execution->isAsyncFunction && state.pendingException.has_value()) {
                    const bool resolved = finishUncaughtInterpreterAsyncException(
                        state, function, activeCoroutine, result);
                    state.valueTop = frameOffset;
                    state.stack.pop_back();
                    return resolved;
                }
                state.valueTop = frameOffset;
                state.stack.pop_back();
                return false;
            }
            if (state.pendingException && execution->hasExceptionHandlers) {
                const auto protectedBlockId = execution->blocks[cold.blockIndex].bytecode->id;
                const auto handlerOffset = routeFastException(protectedBlockId);
                if (handlerOffset != detail::InvalidStreamOffset) {
                    ip = streamBase + handlerOffset;
                    RS_FLAT_DISPATCH_AT_IP();
                }
            }
            if (!state.pendingException && cold.blockIndex < execution->blocks.size()) {
                const bool isTerm =
                    ip->flatOpcode >= detail::FlatOpcode::TermJumpNoArgs ||
                    ip->flatOpcode == detail::FlatOpcode::SuperLessLocalRegBranch ||
                    ip->flatOpcode == detail::FlatOpcode::SuperLessLocalConstBranch ||
                    ip->flatOpcode == detail::FlatOpcode::SuperLessLocalArrayLengthBranch ||
                    ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
                    ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
                    ip->flatOpcode == detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump;
                if (!isTerm) {
                    refineInterpreterFrameSourceLocation(
                        state,
                        location,
                        execution->blocks[cold.blockIndex].bytecode->id,
                        cold.instructionIndexInBlock,
                        ip->span,
                        false);
                }
            }
        }
        if (execution->isAsyncFunction && state.pendingException.has_value()) {
            const bool resolved = finishUncaughtInterpreterAsyncException(
                state, function, activeCoroutine, result);
            state.valueTop = frameOffset;
            state.stack.pop_back();
            return resolved;
        }
        state.valueTop = frameOffset;
        state.stack.pop_back();
        return false;

#if !(defined(__GNUC__) || defined(__clang__))
        }
#endif

        #if defined(__GNUC__) || defined(__clang__)
        #pragma GCC diagnostic pop
        #endif
        #undef RS_FLAT_DISPATCH_NEXT
        #undef RS_FLAT_DISPATCH_AT_IP
        #undef RS_FLAT_CASE
        #undef RS_FLAT_LIKELY
        #undef RS_FLAT_UNLIKELY
    }

    const auto previousFunctionId = state.currentFunctionId;
    state.currentFunctionId = function.symbolId;
    struct RestoreFunctionId {
        State& state;
        semantic::SymbolId previous;
        ~RestoreFunctionId() { state.currentFunctionId = previous; }
    } restoreFunctionId{state, previousFunctionId};
    state.stack.push_back(&location.qualifiedName);
    ++state.statistics.functionCalls;
    state.statistics.maximumCallDepth = std::max(state.statistics.maximumCallDepth, state.stack.size());
    emitTrace(state, TraceEventKind::FunctionEnter);
    ArenaFrameScope frame(state);
    const std::size_t blockArgCount = activeCoroutineFrame
        ? activeCoroutineFrame->savedArguments.size()
        : arguments.size();
    const auto argumentOffset = frame.base();
    const auto localOffset = argumentOffset + blockArgCount;
    const auto registerOffset = localOffset + function.localTypes.size();
    const auto scratchOffset = registerOffset + function.registerTypes.size();
    frame.allocate(
        blockArgCount + function.localTypes.size() +
        function.registerTypes.size() + execution->edgeScratchCount);
    if (activeCoroutineFrame != nullptr) {
        for (std::size_t index = 0; index < blockArgCount; ++index) {
            state.values[argumentOffset + index] = activeCoroutineFrame->savedArguments[index];
        }
        const std::size_t nLocs = std::min(
            activeCoroutineFrame->savedLocals.size(), function.localTypes.size());
        for (std::size_t index = 0; index < nLocs; ++index) {
            state.values[localOffset + index] = activeCoroutineFrame->savedLocals[index];
        }
        const std::size_t nRegs = std::min(
            activeCoroutineFrame->savedRegisters.size(), function.registerTypes.size());
        for (std::size_t index = 0; index < nRegs; ++index) {
            state.values[registerOffset + index] = activeCoroutineFrame->savedRegisters[index];
        }
    } else {
        for (std::size_t index = 0; index < blockArgCount; ++index) {
            state.values[argumentOffset + index] = arguments[index];
        }
    }
    ValueWindow argumentWindow{&state.values, argumentOffset, blockArgCount};
    ValueWindow locals{&state.values, localOffset, function.localTypes.size()};
    ValueWindow registers{
        &state.values, registerOffset, function.registerTypes.size()};
    ValueWindow edgeScratch{
        &state.values, scratchOffset, execution->edgeScratchCount};
    DebugFrameScope debugFrame(state, state.debugger
        ? debug::DebugFrameView{
            location.module,
            &function,
            nullptr,
            argumentWindow.debugView(),
            locals.debugView(),
            registers.debugView(),
        }
        : debug::DebugFrameView{});
    CoroutineFrameHandle* const prevBlockCoroPtr = state.currentBlockVmCoroutinePtr;
    state.currentBlockVmCoroutinePtr = &activeCoroutine;
    struct RestoreBlockCoroPtr {
        State& st;
        CoroutineFrameHandle* prev;
        ~RestoreBlockCoroPtr() { st.currentBlockVmCoroutinePtr = prev; }
    } restoreBlockCoroPtr{state, prevBlockCoroPtr};
    const detail::ExecutionBlock* block = activeCoroutineFrame
        ? detail::findExecutionBlock(*execution, activeCoroutineFrame->resumeBlockId)
        : &execution->blocks[execution->entry];
    if (!block) {
        state.stack.pop_back();
        return fail(state, ErrorCode::InvalidProgram, "invalid coroutine resume block in block VM");
    }

    const auto routePendingException = [&](bytecode::BlockId protectedBlock) {
        if (!state.pendingException ||
            !std::holds_alternative<ObjectRef>(*state.pendingException)) {
            return static_cast<const detail::ExecutionBlock*>(nullptr);
        }
        const auto reference = std::get<ObjectRef>(*state.pendingException);
        const auto actual = state.heap->objectTypeId(reference);
        if (!actual) return static_cast<const detail::ExecutionBlock*>(nullptr);
        for (const auto& handler : function.exceptionHandlers) {
            if (std::find(handler.protectedBlocks.begin(),
                          handler.protectedBlocks.end(), protectedBlock) ==
                handler.protectedBlocks.end()) {
                continue;
            }
            if (handler.catchTypeId != 0 &&
                !runtimeAssignable(state, *actual, handler.catchTypeId)) {
                continue;
            }
            if (handler.exceptionLocal >= locals.size()) {
                return static_cast<const detail::ExecutionBlock*>(nullptr);
            }
            locals[handler.exceptionLocal] = *state.pendingException;
            state.pendingException.reset();
            return detail::findExecutionBlock(*execution, handler.handlerBlock);
        }
        return static_cast<const detail::ExecutionBlock*>(nullptr);
    };

    while (true) {
        const auto& bytecodeBlock = *block->bytecode;
        if (state.debugger && !state.debugFrames.empty()) {
            state.debugFrames.back().activeBlockId = bytecodeBlock.id;
        }
        bool exceptionTransferred = false;
        for (std::size_t instructionIndex = 0;
             instructionIndex < block->instructions.size();
             ++instructionIndex) {
            const auto& decodedInstruction = block->instructions[instructionIndex];
            if (state.debugger && execution->hasDebugSequencePoints &&
                !debugSequencePoint(
                    state, *location.module, function, bytecodeBlock.id,
                    static_cast<std::uint32_t>(instructionIndex), false)) {
                state.stack.pop_back();
                return false;
            }
            std::size_t executionSpan = 1;
            bool executed = false;
            const auto canExecuteSuperInstruction =
                state.fastAccounting &&
                decodedInstruction.superOpcode != detail::SuperOpcode::None &&
                state.executed <= state.limits.instructionBudget &&
                decodedInstruction.span <=
                    state.limits.instructionBudget - state.executed;
            if (canExecuteSuperInstruction) {
                executionSpan = decodedInstruction.span;
                executed = executeSuperInstruction(
                    state, decodedInstruction,
                    argumentWindow, locals, registers);
                if (executed) {
                    stepGcIfNeeded(state, executionSpan);
                }
            } else if (decodedInstruction.opcode ==
                    detail::QuickOpcode::Generic ||
                    decodedInstruction.opcode >= detail::QuickOpcode::ConstantDouble) {
                executed = executeInstruction(
                    state, *location.module, function, *decodedInstruction.bytecode,
                    argumentWindow, locals, registers);
            } else {
                executed = executeQuickInstruction(
                    state, *location.module, decodedInstruction,
                    argumentWindow, locals, registers);
            }
            if (!executed) {
                if (state.pendingException) {
                    if (const auto* handler = routePendingException(bytecodeBlock.id)) {
                        block = handler;
                        exceptionTransferred = true;
                        break;
                    }
                } else {
                    refineInterpreterFrameSourceLocation(
                        state,
                        location,
                        bytecodeBlock.id,
                        static_cast<std::uint32_t>(instructionIndex),
                        static_cast<std::uint32_t>(executionSpan),
                        false);
                }
                if (execution->isAsyncFunction && state.pendingException.has_value()) {
                    const bool resolved = finishUncaughtInterpreterAsyncException(
                        state, function, activeCoroutine, result);
                    state.stack.pop_back();
                    return resolved;
                }
                state.stack.pop_back();
                return false;
            }
            if (state.coroutineSuspendedNow) {
                state.coroutineSuspendedNow = false;
                result = std::move(state.suspendedReturnValue);
                if (state.traceEventsEnabled) {
                    emitTrace(state, TraceEventKind::FunctionExit);
                }
                state.stack.pop_back();
                return true;
            }
            instructionIndex += executionSpan - 1;
        }
        if (exceptionTransferred) continue;
        if ((state.debugger && !debugSequencePoint(
                state, *location.module, function, bytecodeBlock.id,
                static_cast<std::uint32_t>(bytecodeBlock.instructions.size()), true)) ||
            !consume(
                state,
                block->terminatorOpId,
                block->terminatorOpName ? std::string_view(block->terminatorOpName) : std::string_view{})) {
            state.stack.pop_back();
            return false;
        }
        if (state.fastAccounting) {
            if (block->terminatorKind == bytecode::TerminatorKind::Branch &&
                block->conditionPrevalidatedBool) {
                const auto cond = std::get<bool>(registers[block->terminatorReg]);
                const detail::ExecutionEdge* fastEdge = cond ? &block->branch : &block->falseBranch;
                if (!fastEdge->hasArguments && fastEdge->target != detail::InvalidBlockIndex) {
                    ++state.statistics.branchesTaken;
                    block = &execution->blocks[fastEdge->target];
                    continue;
                }
            } else if (block->terminatorKind == bytecode::TerminatorKind::Jump) {
                const detail::ExecutionEdge* fastEdge = &block->branch;
                if (!fastEdge->hasArguments && fastEdge->target != detail::InvalidBlockIndex) {
                    ++state.statistics.branchesTaken;
                    block = &execution->blocks[fastEdge->target];
                    continue;
                }
            } else if (block->terminatorKind == bytecode::TerminatorKind::ReturnValue &&
                       block->returnPrevalidatedScalar) {
                result = registers[block->terminatorReg];
                state.stack.pop_back();
                return true;
            } else if (block->terminatorKind == bytecode::TerminatorKind::ReturnVoid) {
                result = std::monostate{};
                state.stack.pop_back();
                return true;
            }
        }
        const auto& terminator = bytecodeBlock.terminator;
        if (terminator.kind == bytecode::TerminatorKind::Throw) {
            if (terminator.value >= registers.size() ||
                !std::holds_alternative<ObjectRef>(registers[terminator.value])) {
                const bool failed = fail(state, ErrorCode::NullReference,
                    "throw requires a non-null script object");
                state.stack.pop_back();
                return failed;
            }
            if (state.debugger) {
                const auto excRef = std::get<ObjectRef>(registers[terminator.value]);
                semantic::SymbolId actualTypeId = 0;
                if (state.heap) {
                    if (const auto tid = state.heap->objectTypeId(excRef)) {
                        actualTypeId = *tid;
                    }
                }
                const auto* typeDesc = (actualTypeId != 0 && state.program)
                    ? state.program->resolveType(actualTypeId)
                    : nullptr;
                std::string excTypeName = typeDesc && !typeDesc->name.empty()
                    ? typeDesc->name
                    : "Exception";
                std::string excDescription = excTypeName;
                if (typeDesc && state.heap) {
                    for (std::size_t fieldIdx = 0; fieldIdx < typeDesc->fields.size(); ++fieldIdx) {
                        if (typeDesc->fields[fieldIdx].type == semantic::PrimitiveType::String) {
                            const auto fieldVal = state.heap->fieldGet(excRef, fieldIdx);
                            if (fieldVal && !std::holds_alternative<NullString>(*fieldVal) &&
                                !std::holds_alternative<std::monostate>(*fieldVal)) {
                                const auto msg = valueToString(*fieldVal, state.heap);
                                if (!msg.empty()) {
                                    excDescription = excTypeName + ": " + msg;
                                }
                                break;
                            }
                        }
                    }
                }
                bool isUncaught = true;
                if (actualTypeId != 0) {
                    for (auto frameIt = state.debugFrames.rbegin();
                         frameIt != state.debugFrames.rend();
                         ++frameIt) {
                        if (!frameIt->function) continue;
                        bool frameCatches = false;
                        for (const auto& handler : frameIt->function->exceptionHandlers) {
                            if (std::find(
                                    handler.protectedBlocks.begin(),
                                    handler.protectedBlocks.end(),
                                    frameIt->activeBlockId) ==
                                handler.protectedBlocks.end()) {
                                continue;
                            }
                            if (handler.catchTypeId != 0 &&
                                !runtimeAssignable(state, actualTypeId, handler.catchTypeId)) {
                                continue;
                            }
                            frameCatches = true;
                            break;
                        }
                        if (frameCatches) {
                            isUncaught = false;
                            break;
                        }
                    }
                }
                if (!notifyDebuggerException(
                        state,
                        ErrorCode::ScriptException,
                        std::move(excTypeName),
                        std::move(excDescription),
                        isUncaught,
                        false)) {
                    state.stack.pop_back();
                    return fail(state, ErrorCode::ExecutionTerminated,
                        "execution terminated by debugger");
                }
            }
            state.pendingException = registers[terminator.value];
            if (const auto* handler = routePendingException(bytecodeBlock.id)) {
                block = handler;
                continue;
            }
            if (execution->isAsyncFunction && state.pendingException.has_value()) {
                const bool resolved = finishUncaughtInterpreterAsyncException(
                    state, function, activeCoroutine, result);
                state.stack.pop_back();
                return resolved;
            }
            state.stack.pop_back();
            return false;
        }
        if (terminator.kind == bytecode::TerminatorKind::ReturnVoid) {
            result = std::monostate{};
            if (state.traceEventsEnabled) {
                if (state.diagnosticEventsEnabled) {
                    const auto operation = valueToString(result, state.heap);
                    emitTrace(state, TraceEventKind::FunctionExit, 0, operation);
                } else {
                    emitTrace(state, TraceEventKind::FunctionExit);
                }
            }
            state.stack.pop_back();
            return true;
        }
        if (terminator.kind == bytecode::TerminatorKind::ReturnValue) {
            if (terminator.value >= registers.size()) {
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram, "return register is invalid");
            }
            if (!expectSignatureType(
                    state,
                    registers[terminator.value],
                    function.returnType,
                    function.returnTypeId,
                    "return value")) {
                state.stack.pop_back();
                return false;
            }
            result = registers[terminator.value];
            if (state.traceEventsEnabled) {
                if (state.diagnosticEventsEnabled) {
                    const auto operation = valueToString(result, state.heap);
                    emitTrace(state, TraceEventKind::FunctionExit, 0, operation);
                } else {
                    emitTrace(state, TraceEventKind::FunctionExit);
                }
            }
            state.stack.pop_back();
            return true;
        }

        bytecode::BlockId target = terminator.target;
        const detail::ExecutionEdge* edge = &block->branch;
        if (terminator.kind == bytecode::TerminatorKind::Branch) {
            if (terminator.condition >= registers.size() ||
                !expectType(state, registers[terminator.condition],
                    semantic::PrimitiveType::Bool, "branch")) {
                state.stack.pop_back();
                return false;
            }
            if (!std::get<bool>(registers[terminator.condition])) {
                target = terminator.falseTarget;
                edge = &block->falseBranch;
            }
        } else if (terminator.kind != bytecode::TerminatorKind::Jump) {
            state.stack.pop_back();
            return fail(state, ErrorCode::InvalidProgram, "block has invalid terminator");
        }

        ++state.statistics.branchesTaken;
        if (state.traceEventsEnabled) {
            if (state.determinismOnly) {
                state.determinismEventDigest = accumulateDeterminismEvent(
                    state.determinismEventDigest,
                    TraceEventKind::Branch,
                    state.currentFunctionId,
                    target,
                    state.executed,
                    state.stack.size());
                ++state.determinismEventCount;
            } else if (state.diagnosticEventsEnabled) {
                const auto operation = "bb" + std::to_string(target);
                emitTrace(state, TraceEventKind::Branch, target, operation);
            } else {
                emitTrace(state, TraceEventKind::Branch, target);
            }
        }
        if (edge->target == detail::InvalidBlockIndex ||
            !edge->sources || !edge->destinations ||
            edge->sources->size() != edge->destinations->size()) {
            state.stack.pop_back();
            return fail(state, ErrorCode::InvalidProgram, "branch target or argument count is invalid");
        }
        const auto& sources = *edge->sources;
        const auto& destinations = *edge->destinations;
        if (sources.size() > edgeScratch.size()) {
            state.stack.pop_back();
            return fail(state, ErrorCode::InvalidProgram,
                "branch scratch space is too small");
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            if (sources[index] >= registers.size() ||
                destinations[index].target >= registers.size()) {
                state.stack.pop_back();
                return fail(state, ErrorCode::InvalidProgram,
                    "branch argument register is invalid");
            }
            edgeScratch[index] = registers[sources[index]];
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            registers[destinations[index].target] = std::move(edgeScratch[index]);
        }
        block = &execution->blocks[edge->target];
    }
}


void resumeInterpreterCoroutineCallback(
    void* runnerContext,
    CoroutineFrameHandle frameHandle,
    CoroutineFrame& frame,
    AsyncRuntimeContext& asyncContext) {
    detail::ActiveMutatorScope activeMutatorScope;
    const auto faultUnresolvedFrame = [&](const RuntimeError& err) {
        if (auto* f = asyncContext.coroutines().resolve(frameHandle)) {
            if (f->invalidated) {
                f->completed = true;
                (void)asyncContext.awaitables().trySetCanceled(f->completionHandle);
                return;
            }
            if (!f->completed && !f->suspended) {
                f->completed = true;
                const ErrorCode code = (err.code != ErrorCode::None)
                    ? err.code
                    : ErrorCode::InvalidProgram;
                const std::string msg = !err.message.empty()
                    ? err.message
                    : std::string("resumed coroutine failed with runtime error");
                (void)asyncContext.awaitables().trySetFaulted(
                    f->completionHandle, code, msg);
            }
        }
    };

    if (void* activeRaw = asyncContext.activeInterpreterState()) {
        auto* activeState = static_cast<State*>(activeRaw);
        if (activeState->program != nullptr) {
            const auto* loc = activeState->program->resolveFunction(frame.functionSymbolId);
            if (loc) {
                const CoroutineFrameHandle savedCoro = activeState->resumingCoroutine;
                CoroutineFrame* const savedFramePtr = activeState->resumingFramePtr;
                const bool savedSuspendedNow = activeState->coroutineSuspendedNow;
                Value savedSuspendedRet = std::move(activeState->suspendedReturnValue);
                RuntimeError savedError = std::move(activeState->error);
                std::optional<Value> savedPendingExc = std::move(activeState->pendingException);

                activeState->resumingCoroutine = frameHandle;
                activeState->resumingFramePtr = &frame;
                activeState->coroutineSuspendedNow = false;
                activeState->suspendedReturnValue = Value{};
                activeState->error = RuntimeError{};
                activeState->pendingException.reset();

                Value ignoredResult{};
                const ArgumentSource emptyArgs{};
                const bool ok = executeFunction(*activeState, *loc, emptyArgs, ignoredResult);
                if (!ok) {
                    faultUnresolvedFrame(activeState->error);
                }

                activeState->resumingCoroutine = savedCoro;
                activeState->resumingFramePtr = savedFramePtr;
                activeState->coroutineSuspendedNow = savedSuspendedNow;
                activeState->suspendedReturnValue = std::move(savedSuspendedRet);
                activeState->error = std::move(savedError);
                activeState->pendingException = std::move(savedPendingExc);
                return;
            }
        }
    }

    const std::shared_ptr<const void> keepEnvAlive = frame.runnerOwner;
    auto* env = static_cast<InterpreterResumeEnv*>(runnerContext);
    if (!env || !env->program) return;
    const auto* found = env->program->resolveFunction(frame.functionSymbolId);
    if (!found) return;

    std::array<InlineCallCacheEntry, 64> fallbackInlineCache;
    std::vector<PolymorphicPicSite> fallbackPolyCache;
    std::vector<DelegateCallSiteCache> fallbackDelCache;
    const auto& activeMirror = detail::executionMirror(*env->program);

    State state;
    state.mirror = &activeMirror;
    const std::uint64_t mirrorId = activeMirror.instanceId;
    const std::size_t polyCount = activeMirror.polyCallSites.size();
    const std::size_t delCount = activeMirror.invokeDelegateSites.size();
    const bool useTls = (tlsInvokeScratch.depth < ThreadInvokeScratch::MaxDepth);
    if (useTls) {
        auto& buf = tlsInvokeScratch.pool[tlsInvokeScratch.depth++];
        if (buf.values.size() < 256) buf.values.resize(256);
        if (buf.stack.capacity() < 32) buf.stack.reserve(32);
        if (buf.lastMirrorId != mirrorId ||
            buf.polyCallCache.size() != polyCount ||
            buf.delegateCallCache.size() != delCount) {
            buf.inlineCallCache.fill(InlineCallCacheEntry{});
            buf.polyCallCache.assign(polyCount, PolymorphicPicSite{});
            buf.delegateCallCache.assign(delCount, DelegateCallSiteCache{});
            buf.lastMirrorId = mirrorId;
        }
        state.values = std::move(buf.values);
        state.stack = std::move(buf.stack);
        state.inlineCallCache = &buf.inlineCallCache;
        state.polyCallCache = &buf.polyCallCache;
        state.delegateCallCache = &buf.delegateCallCache;
    } else {
        fallbackInlineCache.fill(InlineCallCacheEntry{});
        fallbackPolyCache.assign(polyCount, PolymorphicPicSite{});
        fallbackDelCache.assign(delCount, DelegateCallSiteCache{});
        state.inlineCallCache = &fallbackInlineCache;
        state.polyCallCache = &fallbackPolyCache;
        state.delegateCallCache = &fallbackDelCache;
    }

    struct TlsReleaseGuard {
        State& st;
        bool active;
        ~TlsReleaseGuard() noexcept {
            if (active) {
                if (st.values.size() > 4096) st.values.resize(256);
                st.stack.clear();
                auto& b = tlsInvokeScratch.pool[--tlsInvokeScratch.depth];
                b.values = std::move(st.values);
                b.stack = std::move(st.stack);
            }
        }
    } tlsGuard{state, useTls};

    ExecutionOptions resumeOpts = env->options;
    state.limits = resumeOpts.limits;
    state.program = env->program.get();
    state.externalResolver = &env->externalResolver;
    state.bindings = env->bindings.get();
    state.trace = &env->options.trace;
    state.profile = resumeOpts.profile.get();
    state.determinismEventsEnabled =
        resumeOpts.determinism.mode != DeterminismMode::Off;
    state.diagnosticEventsEnabled =
        state.trace && static_cast<bool>(*state.trace);
    state.determinismOnly = state.determinismEventsEnabled &&
        state.profile == nullptr && !state.diagnosticEventsEnabled;
    state.traceEventsEnabled =
        state.determinismEventsEnabled || state.profile != nullptr ||
        state.diagnosticEventsEnabled;
    state.fastAccounting = !state.traceEventsEnabled &&
        resumeOpts.debugger == nullptr;
    state.determinism = DeterminismSession(std::move(resumeOpts.determinism));
    state.heap = env->heap.get();
    state.debugger = resumeOpts.debugger.get();
    state.asyncRuntime = &asyncContext;
    const std::shared_ptr<const void> keepAlive = frame.runnerOwner;
    state.resumeEnvRaw = env;
    state.resumeEnvOwner = &keepAlive;
    state.resumingCoroutine = frameHandle;
    state.resumingFramePtr = &frame;

    void* const prevBridge = asyncContext.activeInterpreterState();
    asyncContext.setActiveInterpreterState(&state);
    struct RestoreBridge {
        AsyncRuntimeContext& ctx;
        void* prev;
        ~RestoreBridge() noexcept { ctx.setActiveInterpreterState(prev); }
    } restoreBridge{asyncContext, prevBridge};

    ShadowFrameScope roots(
        state.shadowStack, nullptr, nullptr, &state.values, &state.valueTop);
    if (state.traceEventsEnabled && state.heap && state.heap->isWallClockCycleActive()) {
        state.heap->finishWallClockCycleForDeterminism(state.shadowStack);
    }
    Value ignoredResult{};
    const ArgumentSource emptyArgs{};
    const bool ok = executeFunction(state, *found, emptyArgs, ignoredResult);
    if (!ok) {
        faultUnresolvedFrame(state.error);
    }
}

} // namespace

Interpreter::Interpreter(std::vector<bytecode::Module> modules)
    : heap_(std::make_shared<ManagedHeap>()),
      asyncRuntime_(std::make_shared<AsyncRuntimeContext>(heap_.get())) {
    auto linked = ProgramImage::link(std::move(modules), programError_);
    if (linked) {
        program_ = std::make_shared<ProgramImage>(std::move(*linked));
    }
}

Interpreter::Interpreter(std::shared_ptr<const ProgramImage> program)
    : Interpreter(std::move(program), std::make_shared<ManagedHeap>()) {}

Interpreter::Interpreter(
    std::shared_ptr<const ProgramImage> program,
    std::shared_ptr<ManagedHeap> heap)
    : program_(std::move(program)),
      heap_(heap ? std::move(heap) : std::make_shared<ManagedHeap>()),
      asyncRuntime_(std::make_shared<AsyncRuntimeContext>(heap_.get())) {
    if (!program_) {
        programError_.code = ErrorCode::InvalidProgram;
        programError_.message = "interpreter has no linked program image";
    }
}

void Interpreter::setExternalResolver(ExternalFunction resolver) {
    externalResolver_ = std::move(resolver);
    resumeEnv_.reset();
}

void Interpreter::setHeap(std::shared_ptr<ManagedHeap> heap) {
    heap_ = heap ? std::move(heap) : std::make_shared<ManagedHeap>();
    if (asyncRuntime_) asyncRuntime_->setHeap(heap_.get());
    resumeEnv_.reset();
}

std::shared_ptr<ManagedHeap> Interpreter::heap() const noexcept { return heap_; }

void Interpreter::setAsyncRuntime(std::shared_ptr<AsyncRuntimeContext> asyncRuntime) {
    asyncRuntime_ = asyncRuntime
        ? std::move(asyncRuntime)
        : std::make_shared<AsyncRuntimeContext>(heap_.get());
    if (heap_ && asyncRuntime_->awaitables().heap() == nullptr) {
        asyncRuntime_->setHeap(heap_.get());
    }
}

std::shared_ptr<AsyncRuntimeContext> Interpreter::asyncRuntime() const noexcept {
    return asyncRuntime_;
}

void Interpreter::setBindingRegistry(std::shared_ptr<const BindingRegistry> bindings) {
    bindings_ = std::move(bindings);
    resumeEnv_.reset();
}

ExecutionResult Interpreter::invoke(
    semantic::SymbolId symbolId,
    const std::vector<Value>& arguments,
    Limits limits) const {
    ExecutionOptions options;
    options.limits = limits;
    return invoke(symbolId, arguments, std::move(options));
}

ExecutionResult Interpreter::invoke(
    semantic::SymbolId symbolId,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    if (!program_) {
        ExecutionResult invalid;
        invalid.error = programError_;
        if (invalid.error.code == ErrorCode::None) {
            invalid.error.code = ErrorCode::InvalidProgram;
            invalid.error.message = "interpreter has no linked program image";
        }
        return invalid;
    }
    const auto* found = program_->resolveFunction(symbolId);
    if (!found) {
        ExecutionResult missing;
        missing.error.code = ErrorCode::FunctionNotFound;
        missing.error.message = "entry function was not found";
        return missing;
    }
    detail::ActiveMutatorScope activeMutatorScope;
    auto& tlsScratch = tlsInvokeScratch;
    std::array<InlineCallCacheEntry, 64> fallbackInlineCache;
    std::vector<PolymorphicPicSite> fallbackPolyCache;
    std::vector<DelegateCallSiteCache> fallbackDelCache;
    const auto& activeMirror = detail::executionMirror(*program_);
    struct ScratchGuard {
        ThreadInvokeScratch& scratch;
        State& state;
        bool active = false;
        ScratchGuard(
            ThreadInvokeScratch& s,
            State& st,
            const detail::ExecutionMirror& mirror,
            std::array<InlineCallCacheEntry, 64>& fallback,
            std::vector<PolymorphicPicSite>& fallbackPoly,
            std::vector<DelegateCallSiteCache>& fallbackDel)
            : scratch(s), state(st) {
            state.mirror = &mirror;
            const std::uint64_t mirrorId = mirror.instanceId;
            const std::size_t polyCount = mirror.polyCallSites.size();
            const std::size_t delCount = mirror.invokeDelegateSites.size();
            if (scratch.depth < ThreadInvokeScratch::MaxDepth) {
                auto& buf = scratch.pool[scratch.depth++];
                if (buf.values.size() < 256) buf.values.resize(256);
                if (buf.stack.capacity() < 32) buf.stack.reserve(32);
                if (buf.lastMirrorId != mirrorId ||
                    buf.polyCallCache.size() != polyCount ||
                    buf.delegateCallCache.size() != delCount) {
                    buf.inlineCallCache.fill(InlineCallCacheEntry{});
                    buf.polyCallCache.assign(polyCount, PolymorphicPicSite{});
                    buf.delegateCallCache.assign(delCount, DelegateCallSiteCache{});
                    buf.lastMirrorId = mirrorId;
                }
                state.values = std::move(buf.values);
                state.stack = std::move(buf.stack);
                state.inlineCallCache = &buf.inlineCallCache;
                state.polyCallCache = &buf.polyCallCache;
                state.delegateCallCache = &buf.delegateCallCache;
                active = true;
            } else {
                fallback.fill(InlineCallCacheEntry{});
                fallbackPoly.assign(polyCount, PolymorphicPicSite{});
                fallbackDel.assign(delCount, DelegateCallSiteCache{});
                state.inlineCallCache = &fallback;
                state.polyCallCache = &fallbackPoly;
                state.delegateCallCache = &fallbackDel;
            }
        }
        ~ScratchGuard() noexcept {
            if (active) {
                if (state.values.size() > 4096) state.values.resize(256);
                state.stack.clear();
                auto& buf = scratch.pool[--scratch.depth];
                buf.values = std::move(state.values);
                buf.stack = std::move(state.stack);
            }
        }
    };

    State state;
    ScratchGuard scratchGuard(
        tlsScratch,
        state,
        activeMirror,
        fallbackInlineCache,
        fallbackPolyCache,
        fallbackDelCache);
    state.limits = options.limits;
    state.program = program_.get();
    state.externalResolver = &externalResolver_;
    state.bindings = bindings_.get();
    state.trace = &options.trace;
    state.profile = options.profile.get();
    state.determinismEventsEnabled =
        options.determinism.mode != DeterminismMode::Off;
    state.diagnosticEventsEnabled =
        state.trace && static_cast<bool>(*state.trace);
    state.determinismOnly = state.determinismEventsEnabled &&
        state.profile == nullptr && !state.diagnosticEventsEnabled;
    state.traceEventsEnabled =
        state.determinismEventsEnabled || state.profile != nullptr ||
        state.diagnosticEventsEnabled;
    state.fastAccounting = !state.traceEventsEnabled &&
        options.debugger == nullptr;
    if (options.enforceCapabilities) {
        options.determinism.enforceCapabilities = true;
        if (options.determinism.grantedCapabilities.empty()) {
            options.determinism.grantedCapabilities = std::move(options.grantedCapabilities);
        } else {
            options.determinism.grantedCapabilities.insert(
                options.grantedCapabilities.begin(),
                options.grantedCapabilities.end());
        }
    }
    if (!resumeEnv_) {
        auto env = std::make_shared<InterpreterResumeEnv>();
        env->program = program_;
        env->heap = heap_;
        env->bindings = bindings_;
        env->externalResolver = externalResolver_;
        env->options = options;
        resumeEnv_ = env;
    } else {
        static_cast<InterpreterResumeEnv*>(resumeEnv_.get())->options = options;
    }
    state.determinism = DeterminismSession(std::move(options.determinism));
    state.heap = heap_.get();
    state.debugger = options.debugger.get();
    state.asyncRuntime = asyncRuntime_.get();
    if (state.asyncRuntime && state.heap && state.asyncRuntime->awaitables().heap() == nullptr) {
        state.asyncRuntime->setHeap(state.heap);
    }
    const std::shared_ptr<const void> resumeOwnerRef = resumeEnv_;
    state.resumeEnvRaw = resumeEnv_.get();
    state.resumeEnvOwner = &resumeOwnerRef;
    void* const prevAsyncBridge = state.asyncRuntime
        ? state.asyncRuntime->activeInterpreterState()
        : nullptr;
    if (state.asyncRuntime) {
        state.asyncRuntime->setActiveInterpreterState(&state);
    }
    struct ActiveStateBridgeScope {
        AsyncRuntimeContext* ctx;
        void* prev;
        ~ActiveStateBridgeScope() noexcept {
            if (ctx) ctx->setActiveInterpreterState(prev);
        }
    } activeStateBridgeScope{state.asyncRuntime, prevAsyncBridge};
    struct DebugExecutionScope {
        debug::DebugController* debugger = nullptr;
        explicit DebugExecutionScope(debug::DebugController* d) : debugger(d) {
            if (debugger) debugger->beginExecution();
        }
        ~DebugExecutionScope() {
            if (debugger) debugger->endExecution();
        }
        DebugExecutionScope(const DebugExecutionScope&) = delete;
        DebugExecutionScope& operator=(const DebugExecutionScope&) = delete;
    } debugExecutionScope(state.debugger);
    if (state.debugger) {
        state.debugger->bindProgram(*program_);
    }
    Value value;
    ExecutionResult execution;
    ShadowFrameScope roots(
        state.shadowStack, nullptr, nullptr, &state.values, &state.valueTop);
    if (state.traceEventsEnabled && state.heap && state.heap->isWallClockCycleActive()) {
        state.heap->finishWallClockCycleForDeterminism(state.shadowStack);
    }
    execution.succeeded = executeFunction(state, *found, arguments, value);
    state.statistics.instructionsExecuted = state.executed;
    if (!execution.succeeded && state.pendingException &&
        state.error.code == ErrorCode::None) {
        state.error.code = ErrorCode::ScriptException;
        state.error.message = "unhandled script exception";
        state.error.stackTrace = materializeStack(state.stack);
        populateInterpreterSourceLocations(state);
    }
    if (state.determinismEventsEnabled) {
        state.determinism.mergeEventDigest(
            state.determinismEventDigest,
            state.determinismEventCount);
    }
    execution.succeeded = state.determinism.finish(
        execution.succeeded,
        value,
        state.error,
        state.statistics);
    mergeProfile(state);
    execution.value = std::move(value);
    execution.error = std::move(state.error);
    execution.instructionsExecuted = state.executed;
    execution.statistics = state.statistics;
    execution.determinismDigest = state.determinism.digest();
    execution.replayEntriesConsumed = state.determinism.replayEntriesConsumed();
    return execution;
}

ExecutionResult Interpreter::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    Limits limits) const {
    ExecutionOptions options;
    options.limits = limits;
    return invoke(qualifiedName, arguments, std::move(options));
}

ExecutionResult Interpreter::invoke(
    const std::string& qualifiedName,
    const std::vector<Value>& arguments,
    ExecutionOptions options) const {
    if (!program_) {
        ExecutionResult invalid;
        invalid.error = programError_;
        if (invalid.error.code == ErrorCode::None) {
            invalid.error.code = ErrorCode::InvalidProgram;
            invalid.error.message = "interpreter has no linked program image";
        }
        return invalid;
    }
    if (const auto symbolId = program_->findFunction(qualifiedName)) {
        return invoke(*symbolId, arguments, std::move(options));
    }
    ExecutionResult missing;
    missing.error.code = ErrorCode::FunctionNotFound;
    missing.error.message = "entry function '" + qualifiedName + "' was not found";
    return missing;
}

const char* errorCodeName(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None: return "none";
    case ErrorCode::FunctionNotFound: return "function-not-found";
    case ErrorCode::InvalidArguments: return "invalid-arguments";
    case ErrorCode::TypeMismatch: return "type-mismatch";
    case ErrorCode::DivisionByZero: return "division-by-zero";
    case ErrorCode::IntegerOverflow: return "integer-overflow";
    case ErrorCode::InstructionBudgetExceeded: return "instruction-budget-exceeded";
    case ErrorCode::RecursionLimitExceeded: return "recursion-limit-exceeded";
    case ErrorCode::ExternalFunctionUnresolved: return "external-function-unresolved";
    case ErrorCode::DuplicateSymbol: return "duplicate-symbol";
    case ErrorCode::InvalidObjectReference: return "invalid-object-reference";
    case ErrorCode::InvalidNativeHandle: return "invalid-native-handle";
    case ErrorCode::NullReference: return "null-reference";
    case ErrorCode::IndexOutOfRange: return "index-out-of-range";
    case ErrorCode::OutOfMemory: return "out-of-memory";
    case ErrorCode::InvalidProgram: return "invalid-program";
    case ErrorCode::ExecutionTerminated: return "execution-terminated";
    case ErrorCode::DeterminismViolation: return "determinism-violation";
    case ErrorCode::ReplayMismatch: return "replay-mismatch";
    case ErrorCode::ScriptException: return "script-exception";
    case ErrorCode::CapabilityDenied: return "capability-denied";
    }
    return "unknown";
}

bool tryConvertNumeric(
    const Value& value,
    semantic::PrimitiveType target,
    Value& result,
    bool checkedArithmetic) noexcept {
    long double number = 0.0L;
    std::uint64_t integerBits = 0;
    bool integerSource = true;
    if (const auto* byteValue = std::get_if<ByteValue>(&value)) {
        number = byteValue->value; integerBits = byteValue->value;
    } else if (const auto* sbyteValue = std::get_if<SByteValue>(&value)) {
        number = sbyteValue->value; integerBits = static_cast<std::uint64_t>(sbyteValue->value);
    } else if (const auto* shortValue = std::get_if<ShortValue>(&value)) {
        number = shortValue->value; integerBits = static_cast<std::uint64_t>(shortValue->value);
    } else if (const auto* ushortValue = std::get_if<UShortValue>(&value)) {
        number = ushortValue->value; integerBits = ushortValue->value;
    } else if (const auto* intValue = std::get_if<std::int64_t>(&value)) {
        number = static_cast<long double>(*intValue); integerBits = static_cast<std::uint64_t>(*intValue);
    } else if (const auto* uintValue = std::get_if<UIntValue>(&value)) {
        number = uintValue->value; integerBits = uintValue->value;
    } else if (const auto* longValue = std::get_if<LongValue>(&value)) {
        number = static_cast<long double>(longValue->value); integerBits = static_cast<std::uint64_t>(longValue->value);
    } else if (const auto* ulongValue = std::get_if<ULongValue>(&value)) {
        number = static_cast<long double>(ulongValue->value); integerBits = ulongValue->value;
    }
    else if (const auto* floatValue = std::get_if<FloatValue>(&value)) number = floatValue->value;
    else if (const auto* doubleValue = std::get_if<double>(&value)) number = *doubleValue;
    else if (const auto* charValue = std::get_if<CharValue>(&value)) {
        number = charValue->value; integerBits = charValue->value;
    }
    else return false;
    if (std::holds_alternative<FloatValue>(value) ||
        std::holds_alternative<double>(value)) integerSource = false;

    if (!checkedArithmetic && semantic::isIntegralType(target)) {
        if (!integerSource) {
            if (!std::isfinite(number)) integerBits = 0;
            else {
                const auto modulus = std::ldexp(1.0L,
                    target == semantic::PrimitiveType::Byte ||
                    target == semantic::PrimitiveType::SByte ? 8 :
                    target == semantic::PrimitiveType::Short ||
                    target == semantic::PrimitiveType::UShort ||
                    target == semantic::PrimitiveType::Char ? 16 :
                    target == semantic::PrimitiveType::Int ||
                    target == semantic::PrimitiveType::UInt ? 32 : 64);
                auto wrapped = std::fmod(std::trunc(number), modulus);
                if (wrapped < 0) wrapped += modulus;
                integerBits = static_cast<std::uint64_t>(wrapped);
            }
        }
        switch (target) {
        case semantic::PrimitiveType::Byte:
            result = ByteValue{static_cast<std::uint8_t>(integerBits)}; return true;
        case semantic::PrimitiveType::SByte: {
            const auto bits = static_cast<std::uint8_t>(integerBits);
            const auto signedValue = bits <= 0x7fU
                ? static_cast<std::int16_t>(bits)
                : static_cast<std::int16_t>(bits) - 0x100;
            result = SByteValue{static_cast<std::int8_t>(signedValue)}; return true;
        }
        case semantic::PrimitiveType::Short: {
            const auto bits = static_cast<std::uint16_t>(integerBits);
            const auto signedValue = bits <= 0x7fffU
                ? static_cast<std::int32_t>(bits)
                : static_cast<std::int32_t>(bits) - 0x10000;
            result = ShortValue{static_cast<std::int16_t>(signedValue)}; return true;
        }
        case semantic::PrimitiveType::UShort:
            result = UShortValue{static_cast<std::uint16_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Char:
            result = CharValue{static_cast<char16_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Int:
            result = wrapInt32(static_cast<std::int64_t>(integerBits)); return true;
        case semantic::PrimitiveType::UInt:
            result = UIntValue{static_cast<std::uint32_t>(integerBits)}; return true;
        case semantic::PrimitiveType::Long:
            result = LongValue{signedFromBits(integerBits)}; return true;
        case semantic::PrimitiveType::ULong:
            result = ULongValue{integerBits}; return true;
        default:
            break;
        }
    }

    if (checkedArithmetic) {
        const auto inRange = [&](long double minimum, long double maximum) {
            return std::isfinite(number) && number >= minimum && number <= maximum;
        };
        switch (target) {
        case semantic::PrimitiveType::Byte:
            if (!inRange(0, 255)) return false;
            break;
        case semantic::PrimitiveType::SByte:
            if (!inRange(-128, 127)) return false;
            break;
        case semantic::PrimitiveType::Short:
            if (!inRange(-32768, 32767)) return false;
            break;
        case semantic::PrimitiveType::UShort:
        case semantic::PrimitiveType::Char:
            if (!inRange(0, 65535)) return false;
            break;
        case semantic::PrimitiveType::Int:
            if (!inRange(
                    std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max())) return false;
            break;
        case semantic::PrimitiveType::UInt:
            if (!inRange(0, std::numeric_limits<std::uint32_t>::max())) return false;
            break;
        case semantic::PrimitiveType::Long:
            if (!inRange(
                    static_cast<long double>(std::numeric_limits<std::int64_t>::min()),
                    static_cast<long double>(std::numeric_limits<std::int64_t>::max()))) return false;
            break;
        case semantic::PrimitiveType::ULong:
            if (!inRange(0, static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max()))) return false;
            break;
        case semantic::PrimitiveType::Float:
            if (!inRange(
                    -std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max())) return false;
            break;
        default:
            break;
        }
    }

    switch (target) {
    case semantic::PrimitiveType::Byte:
        result = ByteValue{static_cast<std::uint8_t>(number)}; return true;
    case semantic::PrimitiveType::SByte:
        result = SByteValue{static_cast<std::int8_t>(number)}; return true;
    case semantic::PrimitiveType::Short:
        result = ShortValue{static_cast<std::int16_t>(number)}; return true;
    case semantic::PrimitiveType::UShort:
        result = UShortValue{static_cast<std::uint16_t>(number)}; return true;
    case semantic::PrimitiveType::Int:
        result = static_cast<std::int64_t>(number); return true;
    case semantic::PrimitiveType::UInt:
        result = UIntValue{static_cast<std::uint32_t>(number)}; return true;
    case semantic::PrimitiveType::Long:
        result = LongValue{static_cast<std::int64_t>(number)}; return true;
    case semantic::PrimitiveType::ULong:
        result = ULongValue{static_cast<std::uint64_t>(number)}; return true;
    case semantic::PrimitiveType::Float:
        result = FloatValue{static_cast<float>(number)}; return true;
    case semantic::PrimitiveType::Double:
        result = static_cast<double>(number); return true;
    case semantic::PrimitiveType::Char:
        result = CharValue{static_cast<char16_t>(number)}; return true;
    default:
        return false;
    }
}

semantic::PrimitiveType valueType(const Value& value) noexcept {
    static constexpr semantic::PrimitiveType kTagPrimitive[28] = {
        semantic::PrimitiveType::Null,   // 0: Monostate
        semantic::PrimitiveType::String, // 1: NullString
        semantic::PrimitiveType::Object, // 2: NullObject
        semantic::PrimitiveType::Array,  // 3: NullArray
        semantic::PrimitiveType::Bool,   // 4: Bool
        semantic::PrimitiveType::Byte,   // 5: Byte
        semantic::PrimitiveType::SByte,  // 6: SByte
        semantic::PrimitiveType::Short,  // 7: Short
        semantic::PrimitiveType::UShort, // 8: UShort
        semantic::PrimitiveType::Int,    // 9: Int64
        semantic::PrimitiveType::UInt,   // 10: UInt
        semantic::PrimitiveType::Long,   // 11: Long
        semantic::PrimitiveType::ULong,  // 12: ULong
        semantic::PrimitiveType::Float,  // 13: Float
        semantic::PrimitiveType::Double, // 14: Double
        semantic::PrimitiveType::Char,   // 15: Char
        semantic::PrimitiveType::Enum,   // 16: EnumInline
        semantic::PrimitiveType::Struct, // 17: Fixed
        semantic::PrimitiveType::Struct, // 18: Fixed16
        semantic::PrimitiveType::Struct, // 19: Vec2
        semantic::PrimitiveType::Struct, // 20: Vec3
        semantic::PrimitiveType::Struct, // 21: QuatInline
        semantic::PrimitiveType::Handle, // 22: NativeHandle
        semantic::PrimitiveType::Enum,   // 23: EnumArena
        semantic::PrimitiveType::Struct, // 24: QuatArena
        semantic::PrimitiveType::Struct, // 25: Struct
        semantic::PrimitiveType::String, // 26: String
        semantic::PrimitiveType::Object, // 27: ObjectRef (refined below)
    };
    const auto t = value.rawTag();
    if (t == ValueTag::ObjectRef) {
        const auto kind = value.asObjectRefUnchecked().kind;
        if (kind == ObjectKind::String) return semantic::PrimitiveType::String;
        if (kind == ObjectKind::Array) return semantic::PrimitiveType::Array;
        return semantic::PrimitiveType::Object;
    }
    const auto idx = static_cast<std::uint8_t>(t);
    return idx < 28u ? kTagPrimitive[idx] : semantic::PrimitiveType::Null;
}

std::string valueToString(const Value& value) {
    return valueToString(value, nullptr);
}

std::string valueToString(const Value& value, const ManagedHeap* heap) {
    if (std::holds_alternative<std::monostate>(value)) return "null";
    if (std::holds_alternative<NullString>(value)) return "null";
    if (std::holds_alternative<NullObject>(value)) return "null";
    if (std::holds_alternative<NullArray>(value)) return "null";
    if (std::holds_alternative<bool>(value)) return std::get<bool>(value) ? "true" : "false";
    if (const auto* number = std::get_if<ByteValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<SByteValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<ShortValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<UShortValue>(&value)) return std::to_string(number->value);
    if (std::holds_alternative<std::int64_t>(value)) return std::to_string(std::get<std::int64_t>(value));
    if (const auto* number = std::get_if<UIntValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<LongValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<ULongValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<FloatValue>(&value)) return std::to_string(number->value);
    if (const auto* number = std::get_if<double>(&value)) return std::to_string(*number);
    if (const auto* character = std::get_if<CharValue>(&value)) return std::to_string(character->value);
    if (EnumValue enumeration{}; value.tryGetEnum(enumeration)) {
        return "<enum:type=0x" + std::to_string(enumeration.typeId) + ":" +
            std::to_string(enumeration.value) + ">";
    }
    if (const auto* fixed = std::get_if<FixedValue>(&value)) {
        return "<Fixed:raw=" + std::to_string(fixed->raw) + ">";
    }
    if (const auto* fixed16 = std::get_if<Fixed16Value>(&value)) {
        return "<Fixed16:raw=" + std::to_string(fixed16->raw) + ">";
    }
    if (const auto* v2 = std::get_if<Vec2Value>(&value)) {
        return "<Vec2:" + std::to_string(v2->x) + "," + std::to_string(v2->y) + ">";
    }
    if (const auto* v3 = std::get_if<Vec3Value>(&value)) {
        return "<Vec3:" + std::to_string(v3->x) + "," + std::to_string(v3->y) + "," +
            std::to_string(v3->z) + ">";
    }
    if (QuatValue q{}; value.tryGetQuat(q)) {
        return "<Quat:" + std::to_string(q.x) + "," + std::to_string(q.y) + "," +
            std::to_string(q.z) + "," + std::to_string(q.w) + ">";
    }
    if (const auto* structure = std::get_if<StructValue>(&value)) {
        return "<struct:type=0x" + std::to_string(structure->typeId) + ":fields=" +
            std::to_string(structure->fieldCount()) + ">";
    }
    if (std::holds_alternative<std::string>(value)) return std::get<std::string>(value);
    if (NativeHandle handle{}; value.tryGetNativeHandle(handle)) {
        return handle.valid()
            ? "<handle:" + std::to_string(handle.slot) + ":" +
                std::to_string(handle.generation) + ":registry=" +
                std::to_string(handle.registryId) + ":type=0x" +
                std::to_string(handle.typeId) + ">"
            : "null";
    }
    const auto reference = std::get<ObjectRef>(value);
    if (heap) {
        const auto text = heap->stringView(reference);
        if (text) return std::string(*text);
    }
    const auto kind = reference.kind == ObjectKind::Array ? "array" : "object";
    return "<" + std::string(kind) + ":" + std::to_string(reference.slot) + ":" +
        std::to_string(reference.generation) + ":heap=" +
        std::to_string(reference.heapId) + ">";
}

} // namespace realscript::runtime
