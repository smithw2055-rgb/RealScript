#include "realscript/runtime/Runtime.h"

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
    } else if (const auto* v = std::get_if<std::int64_t>(&value)) {
        hashScalar(hash, *v);
    } else if (const auto* v = std::get_if<LongValue>(&value)) {
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<double>(&value)) {
        std::uint64_t bits = 0;
        if (canonicalizeFloatingPoint) {
            bits = canonicalDoubleBits(*v);
        } else {
            static_assert(sizeof(bits) == sizeof(*v), "double width changed");
            std::memcpy(&bits, v, sizeof(bits));
        }
        hashScalar(hash, bits);
    } else if (const auto* v = std::get_if<EnumValue>(&value)) {
        hashScalar(hash, v->typeId);
        hashScalar(hash, v->value);
    } else if (const auto* v = std::get_if<StructValue>(&value)) {
        hashScalar(hash, v->typeId);
        const auto count = v->storage
            ? static_cast<std::uint64_t>(v->storage->fields.size())
            : 0;
        hashScalar(hash, count);
        if (v->storage) {
            for (const auto& field : v->storage->fields) {
                hashValue(hash, field, canonicalizeFloatingPoint);
            }
        }
    } else if (const auto* v = std::get_if<std::string>(&value)) {
        hashString(hash, *v);
    } else if (const auto* v = std::get_if<ObjectRef>(&value)) {
        hashScalar(hash, v->slot);
        hashScalar(hash, v->generation);
        hashScalar(hash, v->kind);
    } else if (const auto* v = std::get_if<NativeHandle>(&value)) {
        hashScalar(hash, v->slot);
        hashScalar(hash, v->generation);
        hashScalar(hash, v->typeId);
    }
}

bool replayStable(const Value& value) noexcept {
    if (std::holds_alternative<ObjectRef>(value) ||
        std::holds_alternative<NativeHandle>(value)) {
        return false;
    }
    if (const auto* structure = std::get_if<StructValue>(&value)) {
        if (!structure->storage) return true;
        return std::all_of(
            structure->storage->fields.begin(),
            structure->storage->fields.end(),
            [](const Value& field) { return replayStable(field); });
    }
    return true;
}

std::uint64_t argumentsHash(
    const std::vector<Value>& arguments,
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

void ProfileCollector::record(const TraceEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++totalEvents_;
    auto& profile = functions_[event.function];
    profile.function = event.function;
    profile.maximumCallDepth = std::max(profile.maximumCallDepth, event.callDepth);
    switch (event.kind) {
    case TraceEventKind::FunctionEnter: ++profile.calls; break;
    case TraceEventKind::FunctionExit: ++profile.returns; break;
    case TraceEventKind::Instruction: ++profile.instructions; break;
    case TraceEventKind::Branch: ++profile.branches; break;
    case TraceEventKind::ExternalCall: ++profile.externalCalls; break;
    case TraceEventKind::GcStep: ++profile.gcSteps; break;
    case TraceEventKind::RuntimeError: ++profile.runtimeErrors; break;
    }
}

ExecutionProfile ProfileCollector::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ExecutionProfile profile;
    profile.totalEvents = totalEvents_;
    profile.functions.reserve(functions_.size());
    for (const auto& [name, function] : functions_) {
        (void)name;
        profile.functions.push_back(function);
    }
    return profile;
}

void ProfileCollector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    functions_.clear();
    totalEvents_ = 0;
}

struct DeterminismSession::Impl {
    explicit Impl(DeterminismOptions value)
        : options(std::move(value)) {
        if (options.mode == DeterminismMode::Replay && options.replayLog) {
            replayEntries = options.replayLog->entries();
        }
    }

    DeterminismOptions options;
    std::vector<ExternalCallRecord> replayEntries;
    std::size_t replayCursor = 0;
    std::uint64_t digest = FnvOffset;
};

DeterminismSession::DeterminismSession(DeterminismOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

DeterminismSession::~DeterminismSession() = default;
DeterminismSession::DeterminismSession(DeterminismSession&&) noexcept = default;
DeterminismSession& DeterminismSession::operator=(DeterminismSession&&) noexcept = default;

void DeterminismSession::observe(const TraceEvent& event) {
    if (!impl_) return;
    hashScalar(impl_->digest, static_cast<std::uint8_t>(event.kind));
    hashString(impl_->digest, event.function);
    // Function-exit value strings are diagnostic and may contain process-local
    // heap or native-handle identities. The returned Value is hashed separately
    // by finalize(), so exclude that presentation-only payload from the digest.
    if (event.kind != TraceEventKind::FunctionExit) {
        hashString(impl_->digest, event.operation);
    }
    hashScalar(impl_->digest, event.instructionIndex);
    const auto depth = static_cast<std::uint64_t>(event.callDepth);
    hashScalar(impl_->digest, depth);
}

bool DeterminismSession::invokeExternal(
    const BindingRegistry* bindings,
    const ExternalFunction* fallback,
    const bytecode::FunctionReference& reference,
    const std::vector<Value>& arguments,
    std::optional<Value>& result,
    RuntimeError& error) {
    if (!impl_) return false;
    const auto mode = impl_->options.mode;
    auto policy = BindingDeterminism::NonDeterministic;
    const ExternalFunction* function = fallback;
    if (bindings) {
        if (const auto resolved = bindings->resolve(reference)) {
            function = resolved->function;
            policy = resolved->determinism;
        }
    }
    const auto argumentHash = argumentsHash(
        arguments, impl_->options.canonicalizeFloatingPoint);
    hashScalar(impl_->digest, reference.symbolId);
    hashString(impl_->digest, reference.name);
    hashScalar(impl_->digest, argumentHash);

    if (mode == DeterminismMode::Strict &&
        policy != BindingDeterminism::Deterministic) {
        error.code = ErrorCode::DeterminismViolation;
        error.message = "external function '" + reference.name +
            "' is not allowed in strict deterministic mode";
        return false;
    }

    if (mode == DeterminismMode::Replay &&
        policy != BindingDeterminism::Deterministic) {
        if (!impl_->options.replayLog ||
            impl_->replayCursor >= impl_->replayEntries.size()) {
            error.code = ErrorCode::ReplayMismatch;
            error.message = "replay log ended before external function '" +
                reference.name + "'";
            return false;
        }
        const auto& record = impl_->replayEntries[impl_->replayCursor++];
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
            hashValue(impl_->digest, *result,
                impl_->options.canonicalizeFloatingPoint);
            return true;
        }
        error.code = record.errorCode;
        error.message = record.errorMessage;
        hashScalar(impl_->digest, static_cast<std::uint16_t>(error.code));
        hashString(impl_->digest, error.message);
        return false;
    }

    if (mode == DeterminismMode::Record &&
        policy != BindingDeterminism::Deterministic) {
        if (!impl_->options.replayLog) {
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
        impl_->options.replayLog->append(std::move(record));
    }
    if (!succeeded) {
        error = std::move(invocationError);
        hashScalar(impl_->digest, static_cast<std::uint16_t>(error.code));
        hashString(impl_->digest, error.message);
        return false;
    }
    result = std::move(invocationResult);
    hashValue(impl_->digest, *result,
                impl_->options.canonicalizeFloatingPoint);
    return true;
}

bool DeterminismSession::finish(
    bool succeeded,
    const Value& value,
    RuntimeError& error,
    const RuntimeStatistics& statistics) {
    if (!impl_) return succeeded;
    if (succeeded && impl_->options.mode == DeterminismMode::Replay &&
        impl_->replayCursor != impl_->replayEntries.size()) {
        succeeded = false;
        error.code = ErrorCode::ReplayMismatch;
        error.message = "replay log contains " +
            std::to_string(impl_->replayEntries.size() - impl_->replayCursor) +
            " unconsumed external call entr" +
            (impl_->replayEntries.size() - impl_->replayCursor == 1
                ? "y" : "ies");
    }
    hashScalar(impl_->digest, succeeded);
    if (succeeded) {
        hashValue(impl_->digest, value,
            impl_->options.canonicalizeFloatingPoint);
    } else {
        hashScalar(impl_->digest, static_cast<std::uint16_t>(error.code));
        hashString(impl_->digest, error.message);
        for (const auto& frame : error.stackTrace) hashString(impl_->digest, frame);
    }
    hashScalar(impl_->digest, statistics.instructionsExecuted);
    hashScalar(impl_->digest, statistics.functionCalls);
    hashScalar(impl_->digest, statistics.externalCalls);
    hashScalar(impl_->digest, statistics.branchesTaken);
    const auto depth = static_cast<std::uint64_t>(statistics.maximumCallDepth);
    hashScalar(impl_->digest, depth);
    hashScalar(impl_->digest, statistics.gcWorkPerformed);
    if (impl_->options.mode == DeterminismMode::Replay &&
        impl_->replayCursor != impl_->replayEntries.size()) {
        hashScalar(impl_->digest,
            static_cast<std::uint64_t>(impl_->replayEntries.size() -
                impl_->replayCursor));
    }
    return succeeded;
}

std::uint64_t DeterminismSession::digest() const noexcept {
    return impl_ ? impl_->digest : 0;
}

std::size_t DeterminismSession::replayEntriesConsumed() const noexcept {
    return impl_ ? impl_->replayCursor : 0;
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

} // namespace realscript::runtime
