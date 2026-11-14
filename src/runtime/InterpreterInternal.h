#pragma once

#include <cstring>
#include "realscript/runtime/Runtime.h"
#include "realscript/runtime/AsyncRuntime.h"

#include "ExecutionMirror.h"
#include "RuntimeInternal.h"
#include "realscript/runtime/DebugHook.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realscript::runtime::interp_detail {

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

inline DeterminismOperationId instructionOperationId(
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

[[maybe_unused]] inline DeterminismOperationId terminatorOperationId(
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
    debug::RuntimeDebugHook* debugger = nullptr;
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
    IAwaitScheduler* awaitScheduler = nullptr;
    CrossHeapAccessPolicy crossHeapPolicy = CrossHeapAccessPolicy::AllowReadWrite;
    void* resumeEnvRaw = nullptr;
    const std::shared_ptr<const void>* resumeEnvOwner = nullptr;
    CoroutineFrameHandle resumingCoroutine{};
    CoroutineFrame* resumingFramePtr = nullptr;
    CoroutineFrameHandle* currentBlockVmCoroutinePtr = nullptr;
    bool coroutineSuspendedNow = false;
    Value suspendedReturnValue{};
    FrameTelemetryCollector* telemetry = activeFrameTelemetry();
    std::uint64_t telemetryChildExecuted = 0;
    std::uint8_t flatBankId = 0;
};

struct FlatStackBankScope {
    State& state;
    explicit FlatStackBankScope(State& st) noexcept : state(st) {
        state.flatBankId = detail::acquireFlatStackBank(&state.values);
    }
    ~FlatStackBankScope() noexcept {
        if (state.flatBankId != 0u) {
            detail::releaseFlatStackBank(state.flatBankId);
            state.flatBankId = 0u;
        }
    }
    FlatStackBankScope(const FlatStackBankScope&) = delete;
    FlatStackBankScope& operator=(const FlatStackBankScope&) = delete;
};

inline void storeValueIntoFlatFrameSlot(
    State& state,
    const detail::ExecutionFunction& execution,
    std::size_t frameOffset,
    Value* frameBase,
    std::uint16_t slot,
    const Value& srcVal) {
    if (execution.hasStructPayloads &&
        state.flatBankId != 0u &&
        slot < execution.slotStructSchemaIds.size()) {
        const std::uint16_t sid = execution.slotStructSchemaIds[slot];
        if (sid != 0u) {
            const std::uint16_t payOff = execution.slotPayloadOffsets[slot];
            const auto* sc = getFlatStructSchema(sid);
            if (payOff != detail::InvalidCompactReg && sc != nullptr) {
                if (extractStructLeavesToBuffer(srcVal, *sc, frameBase + payOff)) {
                    frameBase[slot] = StructValue::makeFrameSlice(
                        sc->structTypeId,
                        state.flatBankId,
                        static_cast<std::uint32_t>(frameOffset + payOff),
                        sid);
                    return;
                }
                if (srcVal.rawTag() == ValueTag::Monostate) {
                    std::memcpy(
                        static_cast<void*>(frameBase + payOff),
                        static_cast<const void*>(sc->prototypeLeaves.data()),
                        static_cast<std::size_t>(sc->flatSlotCount) * sizeof(Value));
                    frameBase[slot] = StructValue::makeFrameSlice(
                        sc->structTypeId,
                        state.flatBankId,
                        static_cast<std::uint32_t>(frameOffset + payOff),
                        sid);
                    return;
                }
            }
        }
    }
    frameBase[slot] = srcVal;
}

struct InterpreterResumeEnv {
    std::shared_ptr<const ProgramImage> program;
    std::shared_ptr<ManagedHeap> heap;
    std::shared_ptr<const BindingRegistry> bindings;
    ExternalFunction externalResolver;
    ExecutionOptions options;
    std::weak_ptr<AsyncRuntimeContext> asyncRuntimeWeak;
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
    std::string_view operation = {});

void mergeProfile(State& state);

std::vector<std::string> materializeStack(
    const std::vector<const std::string*>& stack);
void populateInterpreterSourceLocations(State& state);
void refineInterpreterFrameSourceLocation(
    State& state,
    const ProgramImage::FunctionLocation& location,
    bytecode::BlockId blockId,
    std::uint32_t instructionIndex,
    std::uint32_t span,
    bool isTerminator = false);

bool notifyDebuggerException(
    State& state,
    ErrorCode code,
    std::string exceptionId,
    std::string description,
    bool isUncaught,
    bool isRuntimeError);

bool fail(State& state, ErrorCode code, std::string message);

inline void stepGcIfNeeded(State& state, std::uint64_t steps = 1) {
    if (state.limits.gcWorkBudget == 0 || !state.heap) return;
    if (state.traceEventsEnabled && state.heap->isWallClockCycleActive()) {
        state.heap->finishWallClockCycleForDeterminism(state.shadowStack);
    }
    if (state.heap->phase() == GcPhase::Idle && !state.heap->collectionRequested()) {
        return;
    }
    const auto gcSliceStart = state.telemetry
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
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
    if (state.telemetry != nullptr) {
        const auto elapsedNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - gcSliceStart).count());
        state.telemetry->recordGcSliceNanoseconds(elapsedNs);
    }
}

bool consume(
    State& state,
    DeterminismOperationId operationId,
    std::string_view operation = {});

bool expectType(
    State& state,
    const Value& value,
    semantic::PrimitiveType type,
    std::string_view context);

inline const semantic::TypeSymbol* typeDescriptor(
    const bytecode::Module& module,
    std::uint32_t index) noexcept {
    return index < module.types.size() ? &module.types[index] : nullptr;
}

inline semantic::SymbolId typeIdAt(
    const std::vector<semantic::SymbolId>& typeIds,
    std::size_t index) noexcept {
    return index < typeIds.size() ? typeIds[index] : 0;
}

inline std::int64_t wrapInt32(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(value);
    return bits <= static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max())
        ? static_cast<std::int64_t>(bits)
        : static_cast<std::int64_t>(bits) - (std::int64_t{1} << 32);
}

inline std::int64_t signedFromBits(std::uint64_t bits) noexcept {
    return bits <= static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(bits)
        : -1 - static_cast<std::int64_t>(
            std::numeric_limits<std::uint64_t>::max() - bits);
}

bool runtimeAssignable(
    const State& state,
    semantic::SymbolId actual,
    semantic::SymbolId expected) noexcept;

const detail::PrelinkedDispatchTarget* resolvePrelinkedPolymorphicTarget(
    const detail::ExecutionMirror& mirror,
    semantic::SymbolId actualTypeId,
    const detail::PolymorphicCallSiteDesc& site) noexcept;

const semantic::InterfaceDispatchMap* runtimeInterfaceMap(
    const State& state,
    semantic::SymbolId actual,
    semantic::SymbolId interfaceTypeId) noexcept;

bool expectSignatureType(
    State& state,
    const Value& value,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::string_view context);

Value defaultValue(
    const bytecode::Module& module,
    semantic::PrimitiveType type,
    semantic::SymbolId typeId,
    std::size_t depth = 0);

bool valuesEqual(State& state, const Value& left, const Value& right, bool& equal);

#if defined(__GNUC__) || defined(__clang__)
#ifndef RS_QUICK_UNLIKELY
#define RS_QUICK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif
#else
#ifndef RS_QUICK_UNLIKELY
#define RS_QUICK_UNLIKELY(x) (x)
#endif
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

const std::shared_ptr<const std::vector<std::size_t>>& delegateReferenceFields();

bool combineOrRemoveDelegateFastImpl(
    State& state,
    semantic::SymbolId delegateTypeId,
    const Value& left,
    const Value& right,
    bool isRemove,
    Value& result,
    bool* outSlotsMoved = nullptr);

bool resolveDelegateLeafFast(
    State& state,
    const detail::ExecutionMirror& mirror,
    ObjectRef leafRef,
    const ObjectFastView& view,
    std::size_t expectedUserArgs,
    DelegateCallSiteCache& siteCache,
    ResolvedDelegateLeaf& outLeaf) noexcept;

bool flattenAndResolveMulticastFast(
    State& state,
    const detail::ExecutionMirror& mirror,
    semantic::SymbolId delegateTypeId,
    ObjectRef rootRef,
    std::size_t expectedUserArgs,
    DelegateCallSiteCache& siteCache,
    std::vector<ResolvedDelegateLeaf>& outLeaves);

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
    Value& outAwaitableStruct,
    bool runInlineOnComplete = false);

bool suspendInterpreterWithAwaitScheduler(
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
    const Value& awaitableVal,
    Value& outReturnCarrier);

bool resumeInterpreterAwaitable(
    State& state,
    const Value& awaitableVal,
    bool hasResult,
    semantic::PrimitiveType expectedType,
    semantic::SymbolId expectedTypeId,
    Value& outResumedVal);

bool resolveInterpreterCoroutine(
    State& state,
    CoroutineFrameHandle activeCoroutine,
    semantic::SymbolId returnStructTypeId,
    const Value* optionalPayload,
    Value& outWrappedStruct);

bool finishUncaughtInterpreterAsyncException(
    State& state,
    const bytecode::Function& function,
    CoroutineFrameHandle activeCoroutine,
    Value& outResult);

bool executeCall(
    State& state,
    const bytecode::Module& module,
    const bytecode::Instruction& instruction,
    const ValueWindow& registers,
    Value& result);

bool executeQuickInstruction(
    State& state,
    const bytecode::Module& module,
    const detail::ExecutionInstruction& decoded,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers);

bool executeSuperInstruction(
    State& state,
    const detail::ExecutionInstruction& first,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers);

bool executeInstruction(
    State& state,
    const bytecode::Module& module,
    const bytecode::Function& function,
    const bytecode::Instruction& instruction,
    const ValueWindow& arguments,
    ValueWindow locals,
    ValueWindow registers);

bool executeBlockVmFunction(
    State& state,
    const ProgramImage::FunctionLocation& location,
    const detail::ExecutionFunction& executionObj,
    const ArgumentSource& arguments,
    CoroutineFrameHandle& activeCoroutine,
    CoroutineFrame* activeCoroutineFrame,
    Value& result);

bool executeFunction(
    State& state,
    const ProgramImage::FunctionLocation& location,
    const ArgumentSource& arguments,
    Value& result,
    const detail::ExecutionFunction* cachedExecution = nullptr,
    bool argumentsPrevalidated = false,
    std::uint32_t callerStructReturnAbsOffset = detail::InvalidStreamOffset,
    std::uint16_t callerStructReturnSchemaId = 0);

} // namespace realscript::runtime::interp_detail
