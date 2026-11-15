#include "realscript/runtime/Runtime.h"
#include "realscript/runtime/AsyncRuntime.h"
#include "realscript/runtime/FiberScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

namespace realscript::runtime {
namespace {

constexpr std::uint64_t FnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

thread_local FrameTelemetryCollector* gActiveFrameTelemetry = nullptr;
thread_local ScriptAllocationSite gActiveAllocationSite{};

void hashBytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }
}

template <typename T>
void hashScalar(std::uint64_t& hash, T value) noexcept {
    using ValueType = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<ValueType, bool>) {
        const std::uint8_t encoded = value ? 1U : 0U;
        hashBytes(hash, &encoded, sizeof(encoded));
    } else if constexpr (std::is_enum_v<ValueType>) {
        using Scalar = std::underlying_type_t<ValueType>;
        using Unsigned = std::make_unsigned_t<Scalar>;
        const auto encoded = static_cast<Unsigned>(static_cast<Scalar>(value));
        for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
            const auto byte = static_cast<std::uint8_t>(
                (encoded >> (index * 8U)) & static_cast<Unsigned>(0xffU));
            hashBytes(hash, &byte, sizeof(byte));
        }
    } else {
        using Unsigned = std::make_unsigned_t<ValueType>;
        const auto encoded = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
            const auto byte = static_cast<std::uint8_t>(
                (encoded >> (index * 8U)) & static_cast<Unsigned>(0xffU));
            hashBytes(hash, &byte, sizeof(byte));
        }
    }
}

void hashString(std::uint64_t& hash, const std::string& value) noexcept {
    const auto size = static_cast<std::uint64_t>(value.size());
    hashScalar(hash, size);
    hashBytes(hash, value.data(), value.size());
}

std::uint64_t canonicalDoubleBits(double value) noexcept {
    if (std::isnan(value)) return 0x7ff8000000000000ULL;
    if (value == 0.0) return 0;
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double width changed");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void hashValue(
    std::uint64_t& hash,
    const Value& value,
    bool canonicalizeFloatingPoint = true) noexcept {
    const auto tag = static_cast<std::uint64_t>(value.index());
    hashScalar(hash, tag);
    if (std::holds_alternative<std::monostate>(value) ||
        std::holds_alternative<NullString>(value) ||
        std::holds_alternative<NullObject>(value) ||
        std::holds_alternative<NullArray>(value)) {
        return;
    }
    if (const auto* v = std::get_if<bool>(&value)) {
        hashScalar(hash, *v);
    } else if (const auto* v = std::get_if<ByteValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<SByteValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<ShortValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<UShortValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<std::int64_t>(&value)) {
        hashScalar(hash, *v);
    } else if (const auto* v = std::get_if<UIntValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<LongValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<ULongValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<FloatValue>(&value)) {
        std::uint32_t bits = 0;
        if (canonicalizeFloatingPoint && std::isnan(v->value)) {
            bits = 0x7fc00000U;
        } else if (canonicalizeFloatingPoint && v->value == 0.0f) {
            bits = 0;
        } else {
            static_assert(sizeof(bits) == sizeof(v->value), "float width changed");
            std::memcpy(&bits, &v->value, sizeof(bits));
        }
        hashScalar(hash, bits);
    } else if (const auto* v = std::get_if<double>(&value)) {
        std::uint64_t bits = 0;
        if (canonicalizeFloatingPoint) {
            bits = canonicalDoubleBits(*v);
        } else {
            static_assert(sizeof(bits) == sizeof(*v), "double width changed");
            std::memcpy(&bits, v, sizeof(bits));
        }
        hashScalar(hash, bits);
    } else if (const auto* v = std::get_if<CharValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (EnumValue v{}; value.tryGetEnum(v)) {
        hashScalar(hash, v.typeId);
        hashScalar(hash, v.value);
    } else if (const auto* v = std::get_if<FixedValue>(&value)) {
        hashScalar(hash, v->raw);
    } else if (const auto* v = std::get_if<Fixed16Value>(&value)) {
        hashScalar(hash, v->raw);
    } else if (const auto* v = std::get_if<Vec2Value>(&value)) {
        hashScalar(hash, v->x);
        hashScalar(hash, v->y);
    } else if (const auto* v = std::get_if<Vec3Value>(&value)) {
        hashScalar(hash, v->x);
        hashScalar(hash, v->y);
        hashScalar(hash, v->z);
    } else if (QuatValue v{}; value.tryGetQuat(v)) {
        hashScalar(hash, v.x);
        hashScalar(hash, v.y);
        hashScalar(hash, v.z);
        hashScalar(hash, v.w);
    } else if (const auto* v = std::get_if<Fixed64Value>(&value)) {
        hashScalar(hash, v->raw);
    } else if (const auto* v = std::get_if<Vec2fValue>(&value)) {
        auto hashF = [&](float f) {
            const float n = canonicalizeFloatingPoint && f == 0.0f ? 0.0f : f;
            std::uint32_t b = 0;
            std::memcpy(&b, &n, sizeof(b));
            hashScalar(hash, b);
        };
        hashF(v->x); hashF(v->y);
    } else if (const auto* v = std::get_if<Vec3fValue>(&value)) {
        auto hashF = [&](float f) {
            const float n = canonicalizeFloatingPoint && f == 0.0f ? 0.0f : f;
            std::uint32_t b = 0;
            std::memcpy(&b, &n, sizeof(b));
            hashScalar(hash, b);
        };
        hashF(v->x); hashF(v->y); hashF(v->z);
    } else if (QuatfValue v{}; value.tryGetQuatf(v)) {
        auto hashF = [&](float f) {
            const float n = canonicalizeFloatingPoint && f == 0.0f ? 0.0f : f;
            std::uint32_t b = 0;
            std::memcpy(&b, &n, sizeof(b));
            hashScalar(hash, b);
        };
        hashF(v.x); hashF(v.y); hashF(v.z); hashF(v.w);
    } else if (const auto* v = std::get_if<StructValue>(&value)) {
        hashScalar(hash, v->typeId);
        const auto count = static_cast<std::uint64_t>(v->fieldCount());
        hashScalar(hash, count);
        for (std::size_t i = 0; i < v->fieldCount(); ++i) {
            if (const auto f = structValueFieldAt(*v, i)) {
                hashValue(hash, *f, canonicalizeFloatingPoint);
            }
        }
    } else if (const auto* v = std::get_if<std::string>(&value)) {
        hashString(hash, *v);
    } else if (const auto* v = std::get_if<ObjectRef>(&value)) {
        hashScalar(hash, v->slot);
        hashScalar(hash, v->generation);
        hashScalar(hash, v->kind);
    } else if (NativeHandle v{}; value.tryGetNativeHandle(v)) {
        hashScalar(hash, v.slot);
        hashScalar(hash, v.generation);
        hashScalar(hash, v.typeId);
    }
}

bool replayStable(const Value& value) noexcept {
    if (std::holds_alternative<ObjectRef>(value) ||
        std::holds_alternative<NativeHandle>(value)) {
        return false;
    }
    if (const auto* structure = std::get_if<StructValue>(&value)) {
        if (structure->isFrameSlice()) {
            const std::size_t cnt = structure->fieldCount();
            for (std::size_t i = 0; i < cnt; ++i) {
                const auto fv = structValueFieldAt(*structure, i);
                if (fv && !replayStable(*fv)) return false;
            }
            return true;
        }
        if (structure->isInline || !structure->storage) return true;
        return std::all_of(
            structure->storage->fields.begin(),
            structure->storage->fields.end(),
            [](const Value& field) { return replayStable(field); });
    }
    return true;
}

std::uint64_t argumentsHash(
    ValueSpan arguments,
    bool canonicalizeFloatingPoint) noexcept {
    auto hash = FnvOffset;
    const auto count = static_cast<std::uint64_t>(arguments.size());
    hashScalar(hash, count);
    for (const auto& argument : arguments) {
        hashValue(hash, argument, canonicalizeFloatingPoint);
    }
    return hash;
}

const char* jsonEscapeCharacter(char value) noexcept {
    switch (value) {
    case '\\': return "\\\\";
    case '"': return "\\\"";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default: return nullptr;
    }
}

std::string escapeJson(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char character : value) {
        if (const auto* escaped = jsonEscapeCharacter(static_cast<char>(character))) {
            out << escaped;
        } else if (character < 0x20) {
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                << static_cast<unsigned int>(character) << std::dec;
        } else {
            out << static_cast<char>(character);
        }
    }
    return out.str();
}

} // namespace

void ReplayLog::append(ExternalCallRecord record) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(std::move(record));
}

std::vector<ExternalCallRecord> ReplayLog::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::size_t ReplayLog::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void ReplayLog::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

namespace {

std::uint32_t currentProfileThreadId() noexcept {
    static std::atomic<std::uint32_t> nextThreadId{1u};
    thread_local const std::uint32_t tlsThreadId =
        nextThreadId.fetch_add(1u, std::memory_order_relaxed);
    return tlsThreadId;
}

} // namespace

ProfileCollector::ProfileCollector()
    : epochStart_(std::chrono::steady_clock::now()),
      ring_(std::make_unique<TraceRingSlot[]>(TraceRingCapacity)) {}

std::uint64_t ProfileCollector::nowSteadyNs() const noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now <= epochStart_) return 0ULL;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - epochStart_).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0ULL;
}

void ProfileCollector::pushRingSlot(
    TraceEventKind kind,
    semantic::SymbolId functionId,
    std::string_view functionName,
    std::uint64_t timestampNs,
    std::uint64_t durationNs,
    std::size_t callDepth,
    bool hasTimestamp,
    bool isSpan,
    bool countInSummary) noexcept {
    if (!ring_) return;
    const std::uint64_t seq = writeCursor_.fetch_add(1ULL, std::memory_order_relaxed);
    TraceRingSlot& slot = ring_[static_cast<std::size_t>(seq & TraceRingMask)];
    const std::uint64_t busyTag = ((seq + 1ULL) << 1ULL) | 1ULL;
    const std::uint64_t readyTag = (seq + 1ULL) << 1ULL;
    slot.version.store(busyTag, std::memory_order_release);
    slot.timestampNs = timestampNs;
    slot.durationNs = durationNs;
    slot.functionId = functionId;
    slot.callDepth = static_cast<std::uint32_t>(
        std::min<std::size_t>(callDepth, std::numeric_limits<std::uint32_t>::max()));
    slot.threadId = currentProfileThreadId();
    slot.kind = kind;
    slot.hasTimestamp = hasTimestamp;
    slot.isSpan = isSpan;
    slot.countInSummary = countInSummary;
    const std::size_t copyLen = std::min<std::size_t>(
        functionName.size(), sizeof(slot.functionName) - 1u);
    slot.nameLength = static_cast<std::uint16_t>(copyLen);
    if (copyLen != 0u) {
        std::memcpy(slot.functionName, functionName.data(), copyLen);
    }
    slot.functionName[copyLen] = '\0';
    slot.version.store(readyTag, std::memory_order_release);
}

void ProfileCollector::record(const TraceEvent& event) {
    const bool trackCallTs =
        event.kind == TraceEventKind::FunctionEnter ||
        event.kind == TraceEventKind::FunctionExit;
    const std::uint64_t tsNs = trackCallTs ? nowSteadyNs() : 0ULL;
    pushRingSlot(
        event.kind,
        0,
        event.function,
        tsNs,
        0ULL,
        event.callDepth,
        trackCallTs,
        false,
        true);
}

void ProfileCollector::recordCallBegin(
    semantic::SymbolId functionId,
    std::string_view functionName,
    std::size_t callDepth) noexcept {
    pushRingSlot(
        TraceEventKind::FunctionEnter,
        functionId,
        functionName,
        nowSteadyNs(),
        0ULL,
        callDepth,
        true,
        false,
        false);
}

void ProfileCollector::recordCallEnd(
    semantic::SymbolId functionId,
    std::string_view functionName,
    std::size_t callDepth,
    std::uint64_t durationNs) noexcept {
    pushRingSlot(
        TraceEventKind::FunctionExit,
        functionId,
        functionName,
        nowSteadyNs(),
        durationNs,
        callDepth,
        true,
        false,
        false);
}

void ProfileCollector::recordTraceSpan(
    semantic::SymbolId functionId,
    std::string_view functionName,
    std::uint64_t beginTimestampNs,
    std::uint64_t durationNs,
    std::size_t callDepth) noexcept {
    pushRingSlot(
        TraceEventKind::FunctionExit,
        functionId,
        functionName,
        beginTimestampNs,
        durationNs,
        callDepth,
        true,
        true,
        false);
}

void ProfileCollector::drainLocked() const {
    if (!ring_) return;
    const std::uint64_t head = writeCursor_.load(std::memory_order_acquire);
    if (readCursor_ >= head) return;
    if (head - readCursor_ > TraceRingCapacity) {
        const std::uint64_t skipped = (head - TraceRingCapacity) - readCursor_;
        totalEvents_ += skipped;
        readCursor_ = head - TraceRingCapacity;
    }
    for (std::uint64_t seq = readCursor_; seq < head; ++seq) {
        const TraceRingSlot& slot = ring_[static_cast<std::size_t>(seq & TraceRingMask)];
        std::uint64_t v1 = 0;
        bool readOk = false;
        for (int retry = 0; retry < 64; ++retry) {
            v1 = slot.version.load(std::memory_order_acquire);
            const std::uint64_t slotSeq = v1 >> 1ULL;
            if ((v1 & 1ULL) == 0ULL && slotSeq >= (seq + 1ULL)) {
                if (slotSeq > (seq + 1ULL)) {
                    break;
                }
                const std::uint64_t tsNs = slot.timestampNs;
                const std::uint64_t durNs = slot.durationNs;
                const semantic::SymbolId fnId = slot.functionId;
                const std::uint32_t depth = slot.callDepth;
                const std::uint32_t tid = slot.threadId;
                const TraceEventKind kind = slot.kind;
                const bool hasTs = slot.hasTimestamp;
                const bool isSpan = slot.isSpan;
                const bool countSummary = slot.countInSummary;
                const std::size_t nLen = std::min<std::size_t>(
                    slot.nameLength, sizeof(slot.functionName) - 1u);
                char nameBuf[sizeof(slot.functionName)]{};
                if (nLen != 0u) {
                    std::memcpy(nameBuf, slot.functionName, nLen);
                }
                std::atomic_thread_fence(std::memory_order_acquire);
                const std::uint64_t v2 = slot.version.load(std::memory_order_acquire);
                if (v1 == v2) {
                    readOk = true;
                    const std::string fnName(nameBuf, nLen);
                    if (countSummary) {
                        ++totalEvents_;
                        auto& profile = functions_[fnName];
                        profile.function = fnName;
                        profile.maximumCallDepth = std::max<std::size_t>(
                            profile.maximumCallDepth, depth);
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
                    if (hasTs) {
                        if (isSpan) {
                            if (traceEvents_.size() < MaxStoredTraceEvents) {
                                traceEvents_.push_back(ProfileTraceEvent{
                                    TraceEventKind::FunctionExit,
                                    tsNs,
                                    durNs,
                                    fnId,
                                    fnName,
                                    depth,
                                    tid,
                                });
                            }
                        } else if (kind == TraceEventKind::FunctionEnter) {
                            openCallsByThread_[tid].push_back(ProfileTraceEvent{
                                TraceEventKind::FunctionEnter,
                                tsNs,
                                0ULL,
                                fnId,
                                fnName,
                                depth,
                                tid,
                            });
                        } else if (kind == TraceEventKind::FunctionExit) {
                            auto& callStack = openCallsByThread_[tid];
                            if (!callStack.empty()) {
                                ProfileTraceEvent completed = std::move(callStack.back());
                                callStack.pop_back();
                                completed.kind = TraceEventKind::FunctionExit;
                                completed.durationNs = tsNs >= completed.timestampNs
                                    ? (tsNs - completed.timestampNs)
                                    : 0ULL;
                                if (completed.function.empty()) {
                                    completed.function = fnName;
                                }
                                if (traceEvents_.size() < MaxStoredTraceEvents) {
                                    traceEvents_.push_back(std::move(completed));
                                }
                            } else if (traceEvents_.size() < MaxStoredTraceEvents) {
                                traceEvents_.push_back(ProfileTraceEvent{
                                    TraceEventKind::FunctionExit,
                                    tsNs,
                                    durNs,
                                    fnId,
                                    fnName,
                                    depth,
                                    tid,
                                });
                            }
                        }
                    }
                    break;
                }
            }
        }
        if (!readOk && (v1 & 1ULL) != 0ULL && (v1 >> 1ULL) == (seq + 1ULL)) {
            break;
        }
        readCursor_ = seq + 1ULL;
    }
}

void ProfileCollector::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    drainLocked();
}

void ProfileCollector::merge(const ExecutionProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    drainLocked();
    totalEvents_ += profile.totalEvents;
    for (const auto& function : profile.functions) {
        auto& merged = functions_[function.function];
        merged.function = function.function;
        merged.calls += function.calls;
        merged.returns += function.returns;
        merged.instructions += function.instructions;
        merged.branches += function.branches;
        merged.externalCalls += function.externalCalls;
        merged.gcSteps += function.gcSteps;
        merged.runtimeErrors += function.runtimeErrors;
        merged.maximumCallDepth = std::max(
            merged.maximumCallDepth, function.maximumCallDepth);
    }
    for (const auto& ev : profile.traceEvents) {
        if (traceEvents_.size() >= MaxStoredTraceEvents) break;
        traceEvents_.push_back(ev);
    }
}

ExecutionProfile ProfileCollector::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    drainLocked();
    ExecutionProfile profile;
    profile.totalEvents = totalEvents_;
    profile.functions.reserve(functions_.size());
    for (const auto& [name, function] : functions_) {
        (void)name;
        profile.functions.push_back(function);
    }
    std::size_t openCount = 0;
    for (const auto& [tid, stack] : openCallsByThread_) {
        (void)tid;
        openCount += stack.size();
    }
    profile.traceEvents.reserve(traceEvents_.size() + openCount);
    profile.traceEvents.insert(
        profile.traceEvents.end(), traceEvents_.begin(), traceEvents_.end());
    for (const auto& [tid, stack] : openCallsByThread_) {
        (void)tid;
        for (const auto& openEv : stack) {
            if (profile.traceEvents.size() >= MaxStoredTraceEvents) break;
            profile.traceEvents.push_back(openEv);
        }
    }
    return profile;
}

void ProfileCollector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ring_) {
        for (std::size_t i = 0; i < TraceRingCapacity; ++i) {
            ring_[i].version.store(0ULL, std::memory_order_relaxed);
        }
    }
    writeCursor_.store(0ULL, std::memory_order_relaxed);
    readCursor_ = 0ULL;
    functions_.clear();
    traceEvents_.clear();
    openCallsByThread_.clear();
    totalEvents_ = 0;
    epochStart_ = std::chrono::steady_clock::now();
}

// ============================================================================
// Zero-Allocation Lock-Free Per-Frame Ring Telemetry (`FrameTelemetryCollector`)
// ============================================================================

void bindActiveAllocationSite(ScriptAllocationSite site) noexcept {
    gActiveAllocationSite = site;
}

ScriptAllocationSite activeAllocationSite() noexcept {
    return gActiveAllocationSite;
}

void bindActiveFrameTelemetry(FrameTelemetryCollector* collector) noexcept {
    gActiveFrameTelemetry = collector;
}

FrameTelemetryCollector* activeFrameTelemetry() noexcept {
    return gActiveFrameTelemetry;
}

void FrameTelemetryCollector::resetCurrentFrameAccumulators() noexcept {
    for (std::size_t i = 0; i < touchedCount_; ++i) {
        const std::uint8_t idx = touchedBuckets_[i];
        activeBuckets_[idx].symbolId = 0;
        activeBuckets_[idx].instructions = 0;
    }
    touchedCount_ = 0;
    for (std::size_t i = 0; i < touchedAllocCount_; ++i) {
        const std::uint8_t idx = touchedAllocBuckets_[i];
        activeAllocBuckets_[idx] = AllocBucket{};
    }
    touchedAllocCount_ = 0;
    currentTotalInstructions_ = 0;
    currentFiberInstructions_ = 0;
    currentGcSliceNs_ = 0;
    currentAllocCount_ = 0;
    currentAllocBytes_ = 0;
    explicitCompactBytes_ = 0;
    explicitBoxedBytes_ = 0;
    hasExplicitHeapBytes_ = false;
    explicitActiveFibers_ = 0;
    hasExplicitActiveFibers_ = false;
}

void FrameTelemetryCollector::beginFrame(std::uint64_t frameNumber) noexcept {
    resetCurrentFrameAccumulators();
    currentFrameNumber_ = frameNumber;
}

void FrameTelemetryCollector::recordFunctionInstructions(
    semantic::SymbolId symbolId,
    std::uint64_t instructions) noexcept {
    if (instructions == 0) return;
    currentTotalInstructions_ += instructions;
    if (symbolId == 0) return;

    constexpr std::size_t kMask = kAccumulatorBuckets - 1u;
    std::size_t idx = static_cast<std::size_t>((symbolId * 0x9E3779B97F4A7C15ULL) >> 56u) & kMask;
    std::size_t minSlot = idx;
    std::uint64_t minCount = std::numeric_limits<std::uint64_t>::max();

    for (std::size_t probe = 0; probe < kAccumulatorBuckets; ++probe) {
        auto& bucket = activeBuckets_[idx];
        if (bucket.symbolId == symbolId) {
            bucket.instructions += instructions;
            return;
        }
        if (bucket.symbolId == 0 && bucket.instructions == 0) {
            bucket.symbolId = symbolId;
            bucket.instructions = instructions;
            if (touchedCount_ < touchedBuckets_.size()) {
                touchedBuckets_[touchedCount_++] = static_cast<std::uint8_t>(idx);
            }
            return;
        }
        if (bucket.instructions < minCount) {
            minCount = bucket.instructions;
            minSlot = idx;
        }
        idx = (idx + 1u) & kMask;
    }

    // Space-Saving heavy-hitter replacement if >256 distinct functions execute in 1 frame
    if (instructions > minCount) {
        activeBuckets_[minSlot].symbolId = symbolId;
        activeBuckets_[minSlot].instructions = instructions;
    }
}

void FrameTelemetryCollector::recordAllocation(
    semantic::SymbolId symbolId,
    std::uint32_t instructionOffset,
    ObjectKind kind,
    std::size_t sizeBytes) noexcept {
    ++currentAllocCount_;
    const auto bytes64 = static_cast<std::uint64_t>(sizeBytes);
    currentAllocBytes_ += bytes64;

    constexpr std::size_t kMask = kAccumulatorBuckets - 1u;
    const std::uint64_t mixed =
        (symbolId * 0x9E3779B97F4A7C15ULL) ^
        (static_cast<std::uint64_t>(instructionOffset) * 0xBF58476D1CE4E5B9ULL) ^
        (static_cast<std::uint64_t>(static_cast<std::uint8_t>(kind)) * 0x94D049BB133111EBULL);
    std::size_t idx = static_cast<std::size_t>((mixed ^ (mixed >> 32u)) >> 56u) & kMask;
    std::size_t minSlot = idx;
    std::uint64_t minBytes = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t minCount = std::numeric_limits<std::uint32_t>::max();

    for (std::size_t probe = 0; probe < kAccumulatorBuckets; ++probe) {
        auto& bucket = activeAllocBuckets_[idx];
        if (bucket.occupied &&
            bucket.symbolId == symbolId &&
            bucket.instructionOffset == instructionOffset &&
            bucket.kind == kind) {
            if (bucket.allocationCount < std::numeric_limits<std::uint32_t>::max()) {
                ++bucket.allocationCount;
            }
            bucket.allocatedBytes += bytes64;
            return;
        }
        if (!bucket.occupied) {
            bucket.occupied = true;
            bucket.symbolId = symbolId;
            bucket.instructionOffset = instructionOffset;
            bucket.kind = kind;
            bucket.allocationCount = 1u;
            bucket.allocatedBytes = bytes64;
            if (touchedAllocCount_ < touchedAllocBuckets_.size()) {
                touchedAllocBuckets_[touchedAllocCount_++] = static_cast<std::uint8_t>(idx);
            }
            return;
        }
        if (bucket.allocatedBytes < minBytes ||
            (bucket.allocatedBytes == minBytes && bucket.allocationCount < minCount)) {
            minBytes = bucket.allocatedBytes;
            minCount = bucket.allocationCount;
            minSlot = idx;
        }
        idx = (idx + 1u) & kMask;
    }

    if (bytes64 > minBytes || (bytes64 == minBytes && 1u > minCount)) {
        auto& victim = activeAllocBuckets_[minSlot];
        victim.symbolId = symbolId;
        victim.instructionOffset = instructionOffset;
        victim.kind = kind;
        victim.allocationCount = 1u;
        victim.allocatedBytes = bytes64;
        victim.occupied = true;
    }
}

void FrameTelemetryCollector::recordFiberStep(
    std::uint64_t fiberInstructions,
    std::uint32_t activeFibers) noexcept {
    currentFiberInstructions_ += fiberInstructions;
    currentTotalInstructions_ += fiberInstructions;
    explicitActiveFibers_ = activeFibers;
    hasExplicitActiveFibers_ = true;
}

void FrameTelemetryCollector::recordGcSliceNanoseconds(std::uint64_t elapsedNs) noexcept {
    currentGcSliceNs_ += elapsedNs;
}

void FrameTelemetryCollector::setHeapLiveBytes(
    std::size_t compactLiveBytes,
    std::size_t boxedLiveBytes) noexcept {
    explicitCompactBytes_ = compactLiveBytes;
    explicitBoxedBytes_ = boxedLiveBytes;
    hasExplicitHeapBytes_ = true;
}

void FrameTelemetryCollector::noteCompactHeapObject(ObjectRef reference) noexcept {
    if (!reference.valid()) return;
    for (std::size_t i = 0; i < trackedCompactCount_; ++i) {
        if (trackedCompactRefs_[i] == reference) return;
    }
    if (trackedCompactCount_ < trackedCompactRefs_.size()) {
        trackedCompactRefs_[trackedCompactCount_++] = reference;
    } else {
        trackedCompactRefs_[compactReplaceCursor_ & (trackedCompactRefs_.size() - 1u)] = reference;
        ++compactReplaceCursor_;
    }
}

FrameTelemetrySample FrameTelemetryCollector::endFrame(
    const ManagedHeap* heap,
    const AsyncRuntimeContext* asyncRuntime,
    const FiberScheduler* fiberScheduler) noexcept {
    FrameTelemetrySample sample{};
    sample.frameNumber = currentFrameNumber_;
    sample.totalInstructions = currentTotalInstructions_;
    sample.fiberInstructions = currentFiberInstructions_;
    sample.gcSliceNanoseconds = currentGcSliceNs_;

    // Extract Top-8 hotspot functions by (instructions DESC, symbolId ASC) with zero heap allocation
    std::uint8_t topCount = 0;
    for (std::size_t i = 0; i < touchedCount_; ++i) {
        const auto& b = activeBuckets_[touchedBuckets_[i]];
        if (b.symbolId == 0 || b.instructions == 0) continue;

        HotFunctionTelemetry candidate{};
        candidate.symbolId = b.symbolId;
        candidate.instructions = b.instructions;
        candidate.instructionShare = sample.totalInstructions > 0
            ? (static_cast<double>(b.instructions) / static_cast<double>(sample.totalInstructions))
            : 0.0;

        if (topCount < FrameTelemetrySample::kMaxHotFunctions) {
            sample.topFunctions[topCount++] = candidate;
        } else {
            const auto& tail = sample.topFunctions[FrameTelemetrySample::kMaxHotFunctions - 1u];
            const bool better = (candidate.instructions > tail.instructions) ||
                (candidate.instructions == tail.instructions && candidate.symbolId < tail.symbolId);
            if (!better) continue;
            sample.topFunctions[FrameTelemetrySample::kMaxHotFunctions - 1u] = candidate;
        }

        // Insertion-sort newly placed element upward (at most 8 elements)
        for (std::size_t k = static_cast<std::size_t>(topCount - 1u); k > 0; --k) {
            const auto& prev = sample.topFunctions[k - 1u];
            const auto& curr = sample.topFunctions[k];
            if (curr.instructions > prev.instructions ||
                (curr.instructions == prev.instructions && curr.symbolId < prev.symbolId)) {
                std::swap(sample.topFunctions[k - 1u], sample.topFunctions[k]);
            } else {
                break;
            }
        }
    }
    // Extract Top-8 allocation sites by (allocatedBytes DESC, allocationCount DESC, symbolId ASC, offset ASC) with zero heap allocation
    const auto allocBetter = [](const HotAllocationTelemetry& a,
                                const HotAllocationTelemetry& b) noexcept -> bool {
        if (a.allocatedBytes != b.allocatedBytes) return a.allocatedBytes > b.allocatedBytes;
        if (a.allocationCount != b.allocationCount) return a.allocationCount > b.allocationCount;
        if (a.symbolId != b.symbolId) return a.symbolId < b.symbolId;
        if (a.instructionOffset != b.instructionOffset) {
            return a.instructionOffset < b.instructionOffset;
        }
        return static_cast<std::uint8_t>(a.kind) < static_cast<std::uint8_t>(b.kind);
    };
    sample.frameAllocCount = currentAllocCount_;
    sample.frameAllocBytes = currentAllocBytes_;
    std::uint8_t topAllocCount = 0;
    for (std::size_t i = 0; i < touchedAllocCount_; ++i) {
        const auto& b = activeAllocBuckets_[touchedAllocBuckets_[i]];
        if (!b.occupied || b.allocationCount == 0) continue;

        HotAllocationTelemetry candidate{};
        candidate.symbolId = b.symbolId;
        candidate.instructionOffset = b.instructionOffset;
        candidate.kind = b.kind;
        candidate.allocationCount = b.allocationCount;
        candidate.allocatedBytes = b.allocatedBytes;

        if (topAllocCount < FrameTelemetrySample::kMaxHotAllocations) {
            sample.topAllocations[topAllocCount++] = candidate;
        } else {
            const auto& tail =
                sample.topAllocations[FrameTelemetrySample::kMaxHotAllocations - 1u];
            if (!allocBetter(candidate, tail)) continue;
            sample.topAllocations[FrameTelemetrySample::kMaxHotAllocations - 1u] = candidate;
        }

        for (std::size_t k = static_cast<std::size_t>(topAllocCount - 1u); k > 0; --k) {
            if (allocBetter(sample.topAllocations[k], sample.topAllocations[k - 1u])) {
                std::swap(sample.topAllocations[k - 1u], sample.topAllocations[k]);
            } else {
                break;
            }
        }
    }
    sample.hotAllocationCount = topAllocCount;

    sample.hotFunctionCount = topCount;

    if (hasExplicitHeapBytes_) {
        sample.compactHeapLiveBytes = explicitCompactBytes_;
        sample.boxedHeapLiveBytes = explicitBoxedBytes_;
    } else {
        const auto poolStats = getRuntimeValuePoolStatistics();
        const std::size_t inlineArenaBytes =
            poolStats.inlineStructActiveSlots * sizeof(InlineStructArenaEntry);
        std::size_t trackedPackedBytes = 0;
        std::size_t totalHeapBytes = 0;
        if (heap != nullptr) {
            totalHeapBytes = heap->liveBytes();
            std::size_t writeIdx = 0;
            for (std::size_t i = 0; i < trackedCompactCount_; ++i) {
                const ObjectRef ref = trackedCompactRefs_[i];
                if (heap->isAlive(ref)) {
                    trackedCompactRefs_[writeIdx++] = ref;
                    if (heap->isPackedArray(ref) ||
                        heap->isInlineRecordObject(ref) ||
                        heap->isRawColumnArray(ref)) {
                        trackedPackedBytes += heap->objectSizeBytes(ref).value_or(0);
                    }
                }
            }
            trackedCompactCount_ = writeIdx;
            if (trackedPackedBytes > totalHeapBytes) {
                trackedPackedBytes = totalHeapBytes;
            }
        }
        sample.compactHeapLiveBytes = trackedPackedBytes + inlineArenaBytes;
        sample.boxedHeapLiveBytes =
            (totalHeapBytes >= trackedPackedBytes ? (totalHeapBytes - trackedPackedBytes) : 0u) +
            poolStats.heapStructActiveSlots * 64u;
    }

    if (asyncRuntime != nullptr) {
        sample.activeCoroutines = static_cast<std::uint32_t>(
            asyncRuntime->coroutines().statistics().activeCount);
    }
    if (fiberScheduler != nullptr) {
        sample.activeFibers = static_cast<std::uint32_t>(fiberScheduler->activeFiberCount());
    } else if (hasExplicitActiveFibers_) {
        sample.activeFibers = explicitActiveFibers_;
    }

    commitSample(sample);
    ++currentFrameNumber_;
    resetCurrentFrameAccumulators();
    return sample;
}

void FrameTelemetryCollector::commitSample(const FrameTelemetrySample& sample) noexcept {
    const std::uint64_t seq = publishedCount_.load(std::memory_order_relaxed);
    RingSlot& slot = ring_[static_cast<std::size_t>(seq) & (kRingCapacity - 1u)];

    const std::uint64_t v0 = slot.version.load(std::memory_order_relaxed);
    slot.version.store(v0 + 1u, std::memory_order_relaxed); // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    slot.sample = sample;
    slot.version.store(v0 + 2u, std::memory_order_release); // even: committed
    publishedCount_.store(seq + 1u, std::memory_order_release);
}

bool FrameTelemetryCollector::tryReadSlot(
    std::size_t slotIndex,
    FrameTelemetrySample& outSample) const noexcept {
    const RingSlot& slot = ring_[slotIndex & (kRingCapacity - 1u)];
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint64_t v1 = slot.version.load(std::memory_order_acquire);
        if ((v1 & 1u) != 0u || v1 == 0u) {
            continue;
        }
        const FrameTelemetrySample snap = slot.sample;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint64_t v2 = slot.version.load(std::memory_order_relaxed);
        if (v1 == v2) {
            outSample = snap;
            return true;
        }
    }
    return false;
}

bool FrameTelemetryCollector::latestSample(FrameTelemetrySample& outSample) const noexcept {
    const std::uint64_t total = publishedCount_.load(std::memory_order_acquire);
    if (total == 0u) return false;
    return tryReadSlot(static_cast<std::size_t>(total - 1u), outSample);
}

std::size_t FrameTelemetryCollector::copyRecentSamples(
    FrameTelemetrySample* outSamples,
    std::size_t maxSamples) const noexcept {
    if (outSamples == nullptr || maxSamples == 0) return 0;
    const std::uint64_t total = publishedCount_.load(std::memory_order_acquire);
    if (total == 0u) return 0;

    const std::size_t available = static_cast<std::size_t>(
        std::min<std::uint64_t>(total, kRingCapacity));
    const std::size_t toRead = std::min(available, maxSamples);
    const std::uint64_t startSeq = total - static_cast<std::uint64_t>(toRead);

    std::size_t written = 0;
    for (std::size_t i = 0; i < toRead; ++i) {
        const std::size_t idx = static_cast<std::size_t>(startSeq + i) & (kRingCapacity - 1u);
        FrameTelemetrySample snap{};
        if (tryReadSlot(idx, snap)) {
            outSamples[written++] = snap;
        }
    }
    return written;
}

std::uint64_t FrameTelemetryCollector::totalFramesPublished() const noexcept {
    return publishedCount_.load(std::memory_order_acquire);
}

void FrameTelemetryCollector::clear() noexcept {
    resetCurrentFrameAccumulators();
    currentFrameNumber_ = 0;
    trackedCompactCount_ = 0;
    compactReplaceCursor_ = 0;
    publishedCount_.store(0, std::memory_order_release);
    for (auto& slot : ring_) {
        slot.version.store(0, std::memory_order_release);
        slot.sample = FrameTelemetrySample{};
    }
}

std::string FrameTelemetryCollector::formatSummary(
    const ProgramImage* program,
    std::size_t maxFrames) const {
    std::array<FrameTelemetrySample, kRingCapacity> buf{};
    const std::size_t count = copyRecentSamples(
        buf.data(), std::min(maxFrames == 0 ? 1u : maxFrames, kRingCapacity));
    std::ostringstream out;
    out << "FrameTelemetryRing(capacity=" << kRingCapacity
        << ", published=" << totalFramesPublished()
        << ", shown=" << count << ")\n";
    if (count == 0) {
        out << "  <no frames recorded>";
        return out.str();
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& f = buf[i];
        out << "[Frame #" << f.frameNumber << "] inst=" << f.totalInstructions
            << " (fiber=" << f.fiberInstructions << ")"
            << " compactHeap=" << f.compactHeapLiveBytes << "B"
            << " boxedHeap=" << f.boxedHeapLiveBytes << "B"
            << " gcAlloc=" << f.frameAllocCount << "(" << f.frameAllocBytes << "B)"
            << " gcSlice=" << f.gcSliceNanoseconds << "ns"
            << " coros=" << f.activeCoroutines
            << " fibers=" << f.activeFibers;
        if (f.hotFunctionCount > 0) {
            out << " top=[";
            for (std::uint8_t h = 0; h < f.hotFunctionCount; ++h) {
                if (h != 0) out << ", ";
                const auto& hf = f.topFunctions[h];
                const auto* fnLoc = program ? program->resolveFunction(hf.symbolId) : nullptr;
                if (fnLoc) {
                    out << fnLoc->qualifiedName << "(0x" << std::hex << hf.symbolId << std::dec << ")";
                } else {
                    out << "sym#" << hf.symbolId;
                }
                out << ":" << hf.instructions << "("
                    << std::fixed << std::setprecision(1) << (hf.instructionShare * 100.0) << "%)";
            }
            out << "]";
        }
        if (f.hotAllocationCount > 0) {
            out << " allocs=[";
            for (std::uint8_t a = 0; a < f.hotAllocationCount; ++a) {
                if (a != 0) out << ", ";
                const auto& ha = f.topAllocations[a];
                const auto* fnLoc = program ? program->resolveFunction(ha.symbolId) : nullptr;
                if (fnLoc) {
                    out << fnLoc->qualifiedName;
                } else {
                    out << "sym#" << ha.symbolId;
                }
                out << "@" << ha.instructionOffset
                    << ":" << objectKindName(ha.kind)
                    << ":" << ha.allocationCount << "(" << ha.allocatedBytes << "B)";
            }
            out << "]";
        }
        if (i + 1 < count) out << '\n';
    }
    return out.str();
}

std::string FrameTelemetryCollector::toJson(
    const ProgramImage* program,
    std::size_t maxFrames) const {
    std::array<FrameTelemetrySample, kRingCapacity> buf{};
    const std::size_t count = copyRecentSamples(
        buf.data(), std::min(maxFrames == 0 ? kRingCapacity : maxFrames, kRingCapacity));
    std::ostringstream out;
    out << "{\"capacity\":" << kRingCapacity
        << ",\"totalFramesPublished\":" << totalFramesPublished()
        << ",\"frames\":[";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) out << ',';
        const auto& f = buf[i];
        out << "{\"frameNumber\":" << f.frameNumber
            << ",\"totalInstructions\":" << f.totalInstructions
            << ",\"fiberInstructions\":" << f.fiberInstructions
            << ",\"compactHeapLiveBytes\":" << f.compactHeapLiveBytes
            << ",\"boxedHeapLiveBytes\":" << f.boxedHeapLiveBytes
            << ",\"frameAllocCount\":" << f.frameAllocCount
            << ",\"frameAllocBytes\":" << f.frameAllocBytes
            << ",\"gcSliceNanoseconds\":" << f.gcSliceNanoseconds
            << ",\"activeCoroutines\":" << f.activeCoroutines
            << ",\"activeFibers\":" << f.activeFibers
            << ",\"topFunctions\":[";
        for (std::uint8_t h = 0; h < f.hotFunctionCount; ++h) {
            if (h != 0) out << ',';
            const auto& hf = f.topFunctions[h];
            const auto* fnLoc = program ? program->resolveFunction(hf.symbolId) : nullptr;
            out << "{\"symbolId\":" << hf.symbolId
                << ",\"name\":\"" << (fnLoc ? escapeJson(fnLoc->qualifiedName) : std::string{}) << "\""
                << ",\"instructions\":" << hf.instructions
                << ",\"share\":" << std::fixed << std::setprecision(6) << hf.instructionShare
                << '}';
        }
        out << "],\"topAllocations\":[";
        for (std::uint8_t a = 0; a < f.hotAllocationCount; ++a) {
            if (a != 0) out << ',';
            const auto& ha = f.topAllocations[a];
            const auto* fnLoc = program ? program->resolveFunction(ha.symbolId) : nullptr;
            out << "{\"symbolId\":" << ha.symbolId
                << ",\"name\":\"" << (fnLoc ? escapeJson(fnLoc->qualifiedName) : std::string{}) << "\""
                << ",\"instructionOffset\":" << ha.instructionOffset
                << ",\"kind\":\"" << objectKindName(ha.kind) << "\""
                << ",\"count\":" << ha.allocationCount
                << ",\"bytes\":" << ha.allocatedBytes
                << '}';
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

DeterminismSession::DeterminismSession(DeterminismOptions options)
    : mode_(options.mode),
      canonicalizeFloatingPoint_(options.canonicalizeFloatingPoint),
      enforceCapabilities_(options.enforceCapabilities),
      replayLog_(std::move(options.replayLog)),
      grantedCapabilities_(
          options.grantedCapabilities.empty()
              ? nullptr
              : std::make_unique<std::unordered_set<std::string>>(
                    std::move(options.grantedCapabilities))),
      replayCursor_(0),
      digest_(FnvOffset) {
    if (mode_ == DeterminismMode::Replay && replayLog_) {
        replayEntries_ = replayLog_->entries();
    }
}

DeterminismSession::~DeterminismSession() = default;
DeterminismSession::DeterminismSession(DeterminismSession&&) noexcept = default;
DeterminismSession& DeterminismSession::operator=(DeterminismSession&&) noexcept = default;

void DeterminismSession::observe(const TraceEvent& event) {
    hashScalar(digest_, static_cast<std::uint8_t>(event.kind));
    hashString(digest_, event.function);
    // Function-exit value strings are diagnostic and may contain process-local
    // heap or native-handle identities. The returned Value is hashed separately
    // by finalize(), so exclude that presentation-only payload from the digest.
    if (event.kind != TraceEventKind::FunctionExit) {
        hashString(digest_, event.operation);
    }
    hashScalar(digest_, event.instructionIndex);
    const auto depth = static_cast<std::uint64_t>(event.callDepth);
    hashScalar(digest_, depth);
}

void DeterminismSession::mergeEventDigest(
    std::uint64_t eventDigest,
    std::uint64_t eventCount) noexcept {
    hashScalar(digest_, eventCount);
    hashScalar(digest_, eventDigest);
}

bool DeterminismSession::invokeExternal(
    const BindingRegistry* bindings,
    const ExternalFunction* fallback,
    const bytecode::FunctionReference& reference,
    ValueSpan arguments,
    std::optional<Value>& result,
    RuntimeError& error) {
    const auto mode = mode_;
    auto policy = BindingDeterminism::NonDeterministic;
    std::string requiredCapability;
    const ExternalFunction* function = fallback;
    bool resolvedFromRegistry = false;
    if (bindings) {
        if (const auto resolved = bindings->resolve(reference)) {
            function = resolved->function;
            policy = resolved->determinism;
            requiredCapability = resolved->requiredCapability;
            resolvedFromRegistry = true;
        }
    }
    if (enforceCapabilities_) {
        if (!resolvedFromRegistry && function && *function) {
            requiredCapability = "Host.UnregisteredExternal";
        }
        if (!requiredCapability.empty() &&
            (!grantedCapabilities_ ||
             grantedCapabilities_->find(requiredCapability) ==
                 grantedCapabilities_->end())) {
            error.code = ErrorCode::CapabilityDenied;
            error.message = "external function '" + reference.name +
                "' requires capability '" + requiredCapability + "'";
            return false;
        }
    }
    std::uint64_t argumentHash = 0;
    if (mode != DeterminismMode::Off) {
        argumentHash = argumentsHash(
            arguments, canonicalizeFloatingPoint_);
        hashScalar(digest_, reference.symbolId);
        hashString(digest_, reference.name);
        hashScalar(digest_, argumentHash);
    }

    if (mode == DeterminismMode::Strict &&
        policy != BindingDeterminism::Deterministic) {
        error.code = ErrorCode::DeterminismViolation;
        error.message = "external function '" + reference.name +
            "' is not allowed in strict deterministic mode";
        return false;
    }

    if (mode == DeterminismMode::Replay &&
        policy != BindingDeterminism::Deterministic) {
        if (!replayLog_ ||
            replayCursor_ >= replayEntries_.size()) {
            error.code = ErrorCode::ReplayMismatch;
            error.message = "replay log ended before external function '" +
                reference.name + "'";
            return false;
        }
        const auto& record = replayEntries_[replayCursor_++];
        if (record.symbolId != reference.symbolId ||
            record.name != reference.name ||
            record.argumentHash != argumentHash) {
            error.code = ErrorCode::ReplayMismatch;
            error.message = "replay entry does not match external function '" +
                reference.name + "'";
            return false;
        }
        if (record.succeeded) {
            result = record.result;
            hashValue(digest_, *result,
                canonicalizeFloatingPoint_);
            return true;
        }
        error.code = record.errorCode;
        error.message = record.errorMessage;
        hashScalar(digest_, static_cast<std::uint16_t>(error.code));
        hashString(digest_, error.message);
        return false;
    }

    if (mode == DeterminismMode::Record &&
        policy != BindingDeterminism::Deterministic) {
        if (!replayLog_) {
            error.code = ErrorCode::DeterminismViolation;
            error.message = "record mode requires a replay log";
            return false;
        }
        if (!std::all_of(
                arguments.begin(), arguments.end(), isReplayStableValue)) {
            error.code = ErrorCode::DeterminismViolation;
            error.message = "external function '" + reference.name +
                "' used a managed or native handle argument that cannot be replayed";
            return false;
        }
    }

    if (!function || !*function) {
        error.code = ErrorCode::ExternalFunctionUnresolved;
        error.message = "external function '" + reference.name + "' is not registered";
        return false;
    }

    RuntimeError invocationError;
    auto invocationResult = (*function)(reference, arguments, invocationError);
    const bool succeeded = invocationResult.has_value();
    if (mode == DeterminismMode::Record &&
        policy != BindingDeterminism::Deterministic) {
        if (succeeded && !isReplayStableValue(*invocationResult)) {
            error.code = ErrorCode::DeterminismViolation;
            error.message = "external function '" + reference.name +
                "' returned a managed or native handle value that cannot be replayed";
            return false;
        }
        ExternalCallRecord record;
        record.symbolId = reference.symbolId;
        record.name = reference.name;
        record.argumentHash = argumentHash;
        record.succeeded = succeeded;
        if (succeeded) record.result = *invocationResult;
        record.errorCode = invocationError.code;
        record.errorMessage = invocationError.message;
        replayLog_->append(std::move(record));
    }
    if (!succeeded) {
        error = std::move(invocationError);
        if (mode != DeterminismMode::Off) {
            hashScalar(digest_, static_cast<std::uint16_t>(error.code));
            hashString(digest_, error.message);
        }
        return false;
    }
    result = std::move(invocationResult);
    if (mode != DeterminismMode::Off) {
        hashValue(digest_, *result,
            canonicalizeFloatingPoint_);
    }
    return true;
}

bool DeterminismSession::finish(
    bool succeeded,
    const Value& value,
    RuntimeError& error,
    const RuntimeStatistics& statistics) {
    if (succeeded && mode_ == DeterminismMode::Replay &&
        replayCursor_ != replayEntries_.size()) {
        succeeded = false;
        error.code = ErrorCode::ReplayMismatch;
        error.message = "replay log contains " +
            std::to_string(replayEntries_.size() - replayCursor_) +
            " unconsumed external call entr" +
            (replayEntries_.size() - replayCursor_ == 1
                ? "y" : "ies");
    }
    hashScalar(digest_, succeeded);
    if (succeeded) {
        hashValue(digest_, value,
            canonicalizeFloatingPoint_);
    } else {
        hashScalar(digest_, static_cast<std::uint16_t>(error.code));
        hashString(digest_, error.message);
        for (const auto& frame : error.stackTrace) hashString(digest_, frame);
    }
    hashScalar(digest_, statistics.instructionsExecuted);
    hashScalar(digest_, statistics.functionCalls);
    hashScalar(digest_, statistics.externalCalls);
    hashScalar(digest_, statistics.branchesTaken);
    const auto depth = static_cast<std::uint64_t>(statistics.maximumCallDepth);
    hashScalar(digest_, depth);
    hashScalar(digest_, statistics.gcWorkPerformed);
    if (mode_ == DeterminismMode::Replay &&
        replayCursor_ != replayEntries_.size()) {
        hashScalar(digest_,
            static_cast<std::uint64_t>(replayEntries_.size() -
                replayCursor_));
    }
    return succeeded;
}

DeterminismMode DeterminismSession::mode() const noexcept {
    return mode_;
}

bool DeterminismSession::recordAsyncDrainBatch(
    std::uint64_t tickSequence,
    std::size_t count,
    RuntimeError& error) {
    static constexpr semantic::SymbolId kAsyncDrainBatchSymbolId = 0xA5D0A100u;
    if (mode_ != DeterminismMode::Record) {
        return true;
    }
    if (!replayLog_) {
        error.code = ErrorCode::DeterminismViolation;
        error.message = "record mode requires a replay log for async completion queue";
        return false;
    }
    ExternalCallRecord record;
    record.symbolId = kAsyncDrainBatchSymbolId;
    record.name = "$async.drain";
    record.argumentHash = tickSequence;
    record.succeeded = true;
    record.result = Value(static_cast<std::int64_t>(count));
    replayLog_->append(std::move(record));

    hashScalar(digest_, kAsyncDrainBatchSymbolId);
    hashScalar(digest_, tickSequence);
    hashScalar(digest_, static_cast<std::uint64_t>(count));
    return true;
}

bool DeterminismSession::replayAsyncDrainBatch(
    std::uint64_t tickSequence,
    std::size_t& outCount,
    RuntimeError& error) {
    static constexpr semantic::SymbolId kAsyncDrainBatchSymbolId = 0xA5D0A100u;
    outCount = 0;
    if (mode_ != DeterminismMode::Replay) {
        return true;
    }
    if (!replayLog_ || replayCursor_ >= replayEntries_.size()) {
        error.code = ErrorCode::ReplayMismatch;
        error.message = "replay log ended before async drain tick " +
            std::to_string(tickSequence);
        return false;
    }
    const auto& record = replayEntries_[replayCursor_++];
    if (record.symbolId != kAsyncDrainBatchSymbolId ||
        record.name != "$async.drain" ||
        record.argumentHash != tickSequence ||
        !record.succeeded) {
        error.code = ErrorCode::ReplayMismatch;
        error.message = "replay entry does not match async drain tick " +
            std::to_string(tickSequence);
        return false;
    }
    const auto* countInt = std::get_if<std::int64_t>(&record.result);
    if (!countInt || *countInt < 0) {
        error.code = ErrorCode::ReplayMismatch;
        error.message = "replay async drain batch count is invalid";
        return false;
    }
    outCount = static_cast<std::size_t>(*countInt);

    hashScalar(digest_, kAsyncDrainBatchSymbolId);
    hashScalar(digest_, tickSequence);
    hashScalar(digest_, static_cast<std::uint64_t>(outCount));
    return true;
}

bool DeterminismSession::recordAsyncCompletion(
    ExternalCallRecord record,
    RuntimeError& error) {
    if (mode_ != DeterminismMode::Record) {
        return true;
    }
    if (!replayLog_) {
        error.code = ErrorCode::DeterminismViolation;
        error.message = "record mode requires a replay log for async completion";
        return false;
    }
    if (record.succeeded && !isReplayStableValue(record.result)) {
        error.code = ErrorCode::DeterminismViolation;
        error.message = "async operation '" + record.name +
            "' produced a non-replayable handle value";
        return false;
    }

    hashScalar(digest_, record.symbolId);
    hashString(digest_, record.name);
    hashScalar(digest_, record.argumentHash);
    hashScalar(digest_, record.succeeded);
    if (record.succeeded) {
        hashValue(digest_, record.result, canonicalizeFloatingPoint_);
    } else {
        hashScalar(digest_, static_cast<std::uint16_t>(record.errorCode));
        hashString(digest_, record.errorMessage);
    }

    replayLog_->append(std::move(record));
    return true;
}

bool DeterminismSession::consumeAsyncCompletion(
    ExternalCallRecord& outRecord,
    RuntimeError& error) {
    if (mode_ != DeterminismMode::Replay) {
        error.code = ErrorCode::InvalidProgram;
        error.message = "consumeAsyncCompletion called outside replay mode";
        return false;
    }
    if (!replayLog_ || replayCursor_ >= replayEntries_.size()) {
        error.code = ErrorCode::ReplayMismatch;
        error.message = "replay log ended before expected async completion entry";
        return false;
    }
    outRecord = replayEntries_[replayCursor_++];

    hashScalar(digest_, outRecord.symbolId);
    hashString(digest_, outRecord.name);
    hashScalar(digest_, outRecord.argumentHash);
    hashScalar(digest_, outRecord.succeeded);
    if (outRecord.succeeded) {
        hashValue(digest_, outRecord.result, canonicalizeFloatingPoint_);
    } else {
        hashScalar(digest_, static_cast<std::uint16_t>(outRecord.errorCode));
        hashString(digest_, outRecord.errorMessage);
    }
    return true;
}

std::uint64_t DeterminismSession::digest() const noexcept {
    return digest_;
}

std::size_t DeterminismSession::replayEntriesConsumed() const noexcept {
    return replayCursor_;
}

std::uint64_t stableValueHash(const Value& value) noexcept {
    auto hash = FnvOffset;
    hashValue(hash, value, true);
    return hash;
}

bool isReplayStableValue(const Value& value) noexcept {
    return replayStable(value);
}

const char* determinismModeName(DeterminismMode mode) noexcept {
    switch (mode) {
    case DeterminismMode::Off: return "off";
    case DeterminismMode::Strict: return "strict";
    case DeterminismMode::Record: return "record";
    case DeterminismMode::Replay: return "replay";
    }
    return "off";
}

const char* bindingDeterminismName(BindingDeterminism determinism) noexcept {
    switch (determinism) {
    case BindingDeterminism::Deterministic: return "deterministic";
    case BindingDeterminism::Recordable: return "recordable";
    case BindingDeterminism::NonDeterministic: return "non-deterministic";
    }
    return "non-deterministic";
}

std::string formatProfile(const ExecutionProfile& profile) {
    std::ostringstream out;
    out << "events=" << profile.totalEvents << '\n';
    for (const auto& function : profile.functions) {
        out << (function.function.empty() ? "<runtime>" : function.function)
            << " calls=" << function.calls
            << " returns=" << function.returns
            << " instructions=" << function.instructions
            << " branches=" << function.branches
            << " external=" << function.externalCalls
            << " gc=" << function.gcSteps
            << " errors=" << function.runtimeErrors
            << " max-depth=" << function.maximumCallDepth << '\n';
    }
    return out.str();
}

std::string profileToJson(const ExecutionProfile& profile) {
    std::ostringstream out;
    out << "{\"totalEvents\":" << profile.totalEvents << ",\"functions\":[";
    for (std::size_t index = 0; index < profile.functions.size(); ++index) {
        if (index != 0) out << ',';
        const auto& function = profile.functions[index];
        out << "{\"function\":\"" << escapeJson(function.function)
            << "\",\"calls\":" << function.calls
            << ",\"returns\":" << function.returns
            << ",\"instructions\":" << function.instructions
            << ",\"branches\":" << function.branches
            << ",\"externalCalls\":" << function.externalCalls
            << ",\"gcSteps\":" << function.gcSteps
            << ",\"runtimeErrors\":" << function.runtimeErrors
            << ",\"maximumCallDepth\":" << function.maximumCallDepth
            << '}';
    }
    out << "]}";
    return out.str();
}

namespace {

void appendMicrosecondsFromNs(std::ostringstream& out, std::uint64_t ns) {
    const std::uint64_t wholeUs = ns / 1000ULL;
    const std::uint64_t fracNs = ns % 1000ULL;
    out << wholeUs;
    if (fracNs != 0ULL) {
        out << '.';
        if (fracNs < 100ULL) out << '0';
        if (fracNs < 10ULL) out << '0';
        std::uint64_t trimmed = fracNs;
        while (trimmed % 10ULL == 0ULL) trimmed /= 10ULL;
        out << trimmed;
    }
}

} // namespace

std::string exportChromeTraceJson(
    const ExecutionProfile& profile,
    const GcStatistics* gcStatistics) {
    std::ostringstream out;
    out << "{\"traceEvents\":[";
    bool first = true;
    const auto comma = [&] {
        if (!first) out << ',';
        first = false;
    };

    comma();
    out << "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"tid\":1,"
        << "\"args\":{\"name\":\"RealScript VM\"}}";

    std::uint64_t maxEndNs = 0ULL;

    if (!profile.traceEvents.empty()) {
        std::vector<std::uint32_t> threadIds;
        threadIds.reserve(4);
        for (const auto& ev : profile.traceEvents) {
            const std::uint32_t tid = ev.threadId != 0u ? ev.threadId : 1u;
            if (std::find(threadIds.begin(), threadIds.end(), tid) == threadIds.end()) {
                threadIds.push_back(tid);
            }
        }
        std::sort(threadIds.begin(), threadIds.end());
        for (const std::uint32_t tid : threadIds) {
            comma();
            out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" << tid
                << ",\"args\":{\"name\":\""
                << (tid == 1u ? std::string("Script Main Thread")
                              : ("Script Worker Thread " + std::to_string(tid)))
                << "\"}}";
        }

        std::unordered_map<std::string, const FunctionProfile*> summaryByName;
        summaryByName.reserve(profile.functions.size());
        for (const auto& fnProf : profile.functions) {
            summaryByName[fnProf.function] = &fnProf;
        }

        std::vector<std::size_t> order(profile.traceEvents.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](std::size_t lhsIdx, std::size_t rhsIdx) {
            const auto& a = profile.traceEvents[lhsIdx];
            const auto& b = profile.traceEvents[rhsIdx];
            if (a.timestampNs != b.timestampNs) return a.timestampNs < b.timestampNs;
            if (a.durationNs != b.durationNs) return a.durationNs > b.durationNs;
            if (a.callDepth != b.callDepth) return a.callDepth < b.callDepth;
            return lhsIdx < rhsIdx;
        });

        for (const std::size_t idx : order) {
            const auto& ev = profile.traceEvents[idx];
            const std::uint32_t tid = ev.threadId != 0u ? ev.threadId : 1u;
            const std::string name = ev.function.empty()
                ? std::string("<runtime>")
                : escapeJson(ev.function);
            const char phase = (ev.kind == TraceEventKind::FunctionEnter) ? 'B' : 'X';
            comma();
            out << "{\"name\":\"" << name << "\",\"cat\":\"script\",\"ph\":\""
                << phase << "\",\"pid\":1,\"tid\":" << tid << ",\"ts\":";
            appendMicrosecondsFromNs(out, ev.timestampNs);
            if (phase == 'X') {
                out << ",\"dur\":";
                appendMicrosecondsFromNs(out, ev.durationNs);
                maxEndNs = std::max(maxEndNs, ev.timestampNs + ev.durationNs);
            } else {
                maxEndNs = std::max(maxEndNs, ev.timestampNs);
            }
            out << ",\"args\":{\"functionId\":" << ev.functionId
                << ",\"callDepth\":" << ev.callDepth;
            if (const auto sumIt = summaryByName.find(ev.function);
                sumIt != summaryByName.end() && sumIt->second != nullptr) {
                const auto* fn = sumIt->second;
                out << ",\"calls\":" << fn->calls
                    << ",\"returns\":" << fn->returns
                    << ",\"instructions\":" << fn->instructions
                    << ",\"branches\":" << fn->branches
                    << ",\"externalCalls\":" << fn->externalCalls
                    << ",\"gcSteps\":" << fn->gcSteps
                    << ",\"runtimeErrors\":" << fn->runtimeErrors
                    << ",\"maximumCallDepth\":" << fn->maximumCallDepth;
            }
            out << "}}";
        }
    } else {
        comma();
        out << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":1,"
            << "\"args\":{\"name\":\"Script Main Thread\"}}";

        std::uint64_t cursorUs = 0;
        for (const auto& function : profile.functions) {
            const std::uint64_t durationUs = std::max<std::uint64_t>(
                1,
                function.instructions + function.externalCalls * 2 + function.gcSteps * 5);
            const std::string name = function.function.empty()
                ? "<runtime>"
                : escapeJson(function.function);
            comma();
            out << "{\"name\":\"" << name << "\",\"cat\":\"script\",\"ph\":\"X\","
                << "\"pid\":1,\"tid\":1,\"ts\":" << cursorUs
                << ",\"dur\":" << durationUs
                << ",\"args\":{\"calls\":" << function.calls
                << ",\"returns\":" << function.returns
                << ",\"instructions\":" << function.instructions
                << ",\"branches\":" << function.branches
                << ",\"externalCalls\":" << function.externalCalls
                << ",\"gcSteps\":" << function.gcSteps
                << ",\"runtimeErrors\":" << function.runtimeErrors
                << ",\"maximumCallDepth\":" << function.maximumCallDepth
                << "}}";
            cursorUs += durationUs;
        }
        maxEndNs = cursorUs * 1000ULL;
    }

    if (gcStatistics) {
        comma();
        out << "{\"name\":\"GC Memory\",\"cat\":\"gc\",\"ph\":\"C\","
            << "\"pid\":1,\"tid\":1,\"ts\":";
        appendMicrosecondsFromNs(out, maxEndNs);
        out << ",\"args\":{\"liveBytes\":" << gcStatistics->liveBytes
            << ",\"peakLiveBytes\":" << gcStatistics->peakLiveBytes
            << ",\"bytesAllocated\":" << gcStatistics->bytesAllocated
            << ",\"bytesReclaimed\":" << gcStatistics->bytesReclaimed
            << "}}";
        comma();
        out << "{\"name\":\"GC Objects\",\"cat\":\"gc\",\"ph\":\"C\","
            << "\"pid\":1,\"tid\":1,\"ts\":";
        appendMicrosecondsFromNs(out, maxEndNs);
        out << ",\"args\":{\"liveObjects\":" << gcStatistics->liveObjects
            << ",\"objectsAllocated\":" << gcStatistics->objectsAllocated
            << ",\"objectsReclaimed\":" << gcStatistics->objectsReclaimed
            << ",\"collectionsCompleted\":" << gcStatistics->collectionsCompleted
            << "}}";
    }

    out << "]}";
    return out.str();
}

} // namespace realscript::runtime
