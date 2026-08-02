#include "realscript/game/GameplayStateCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace realscript::game {
namespace {

constexpr std::uint8_t Magic[] = {'R', 'S', 'G', 'S'};
constexpr std::size_t HeaderSize = 4 + 4 + 8 + 8;

enum class ValueTag : std::uint8_t {
    Null = 0,
    Bool = 1,
    Int = 2,
    Long = 3,
    Double = 4,
    String = 5,
    Entity = 6,
};

void fail(
    GameplayStateError& error,
    GameplayStateErrorCode code,
    std::string message) {
    if (error.code != GameplayStateErrorCode::None) return;
    error.code = code;
    error.message = std::move(message);
}

std::uint64_t canonicalDoubleBits(double value) noexcept {
    if (value == 0.0) return 0;
    if (std::isnan(value)) return 0x7ff8000000000000ULL;
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "unexpected double width");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double doubleFromBits(std::uint64_t bits) noexcept {
    double value = 0.0;
    static_assert(sizeof(bits) == sizeof(value), "unexpected double width");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

class Writer {
public:
    Writer(GameplayStateError& error, GameplayStateCodecLimits limits)
        : error_(error), limits_(limits) {}

    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void u64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            u8(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void boolean(bool value) { u8(value ? 1u : 0u); }
    void number(double value) { u64(canonicalDoubleBits(value)); }

    bool count(std::size_t value, std::size_t maximum, const char* name) {
        if (value > maximum ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            fail(error_, GameplayStateErrorCode::LimitExceeded,
                std::string{name} + " exceeds the codec limit");
            return false;
        }
        u32(static_cast<std::uint32_t>(value));
        return true;
    }

    bool string(const std::string& value) {
        if (!count(value.size(), limits_.maximumStringBytes, "string length")) {
            return false;
        }
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return withinLimit();
    }

    bool value(const GameplayValue& value) {
        if (std::holds_alternative<std::monostate>(value)) {
            u8(static_cast<std::uint8_t>(ValueTag::Null));
        } else if (const auto* item = std::get_if<bool>(&value)) {
            u8(static_cast<std::uint8_t>(ValueTag::Bool));
            boolean(*item);
        } else if (const auto* item = std::get_if<std::int32_t>(&value)) {
            u8(static_cast<std::uint8_t>(ValueTag::Int));
            u32(static_cast<std::uint32_t>(*item));
        } else if (const auto* item = std::get_if<std::int64_t>(&value)) {
            u8(static_cast<std::uint8_t>(ValueTag::Long));
            u64(static_cast<std::uint64_t>(*item));
        } else if (const auto* item = std::get_if<double>(&value)) {
            u8(static_cast<std::uint8_t>(ValueTag::Double));
            number(*item);
        } else if (const auto* item = std::get_if<std::string>(&value)) {
            u8(static_cast<std::uint8_t>(ValueTag::String));
            if (!string(*item)) return false;
        } else {
            u8(static_cast<std::uint8_t>(ValueTag::Entity));
            u64(std::get<EntityId>(value).packed());
        }
        return withinLimit();
    }

    bool arguments(const std::vector<GameplayValue>& arguments) {
        if (!count(
                arguments.size(),
                limits_.maximumArgumentsPerCall,
                "argument count")) {
            return false;
        }
        for (const auto& argument : arguments) {
            if (!value(argument)) return false;
        }
        return true;
    }

    bool withinLimit() {
        if (bytes_.size() > limits_.maximumEncodedBytes) {
            fail(error_, GameplayStateErrorCode::LimitExceeded,
                "encoded gameplay state exceeds the byte limit");
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    GameplayStateError& error_;
    GameplayStateCodecLimits limits_;
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    Reader(
        const std::vector<std::uint8_t>& bytes,
        GameplayStateError& error,
        GameplayStateCodecLimits limits,
        std::size_t begin = 0,
        std::size_t end = std::numeric_limits<std::size_t>::max())
        : bytes_(bytes),
          error_(error),
          limits_(limits),
          position_(begin),
          end_(std::min(end, bytes.size())) {}

    bool u8(std::uint8_t& value) {
        if (position_ >= end_) return truncated();
        value = bytes_[position_++];
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool boolean(bool& value) {
        std::uint8_t encoded = 0;
        if (!u8(encoded)) return false;
        if (encoded > 1) {
            fail(error_, GameplayStateErrorCode::MalformedInput,
                "boolean value is not zero or one");
            return false;
        }
        value = encoded != 0;
        return true;
    }
    bool number(double& value) {
        std::uint64_t bits = 0;
        if (!u64(bits)) return false;
        value = doubleFromBits(bits);
        return true;
    }

    bool count(std::size_t& value, std::size_t maximum, const char* name) {
        std::uint32_t encoded = 0;
        if (!u32(encoded)) return false;
        value = encoded;
        if (value > maximum) {
            fail(error_, GameplayStateErrorCode::LimitExceeded,
                std::string{name} + " exceeds the codec limit");
            return false;
        }
        return true;
    }

    bool string(std::string& value) {
        std::size_t size = 0;
        if (!count(size, limits_.maximumStringBytes, "string length")) return false;
        if (size > end_ - position_) return truncated();
        value.assign(
            reinterpret_cast<const char*>(bytes_.data() + position_), size);
        position_ += size;
        return true;
    }

    bool value(GameplayValue& value) {
        std::uint8_t encodedTag = 0;
        if (!u8(encodedTag)) return false;
        const auto tag = static_cast<ValueTag>(encodedTag);
        switch (tag) {
        case ValueTag::Null:
            value = std::monostate{};
            return true;
        case ValueTag::Bool: {
            bool item = false;
            if (!boolean(item)) return false;
            value = item;
            return true;
        }
        case ValueTag::Int: {
            std::uint32_t item = 0;
            if (!u32(item)) return false;
            value = static_cast<std::int32_t>(item);
            return true;
        }
        case ValueTag::Long: {
            std::uint64_t item = 0;
            if (!u64(item)) return false;
            value = static_cast<std::int64_t>(item);
            return true;
        }
        case ValueTag::Double: {
            double item = 0.0;
            if (!number(item)) return false;
            value = item;
            return true;
        }
        case ValueTag::String: {
            std::string item;
            if (!string(item)) return false;
            value = std::move(item);
            return true;
        }
        case ValueTag::Entity: {
            std::uint64_t item = 0;
            if (!u64(item)) return false;
            value = EntityId::fromPacked(item);
            return true;
        }
        }
        fail(error_, GameplayStateErrorCode::MalformedInput,
            "gameplay value has an unknown tag");
        return false;
    }

    bool arguments(std::vector<GameplayValue>& arguments) {
        std::size_t countValue = 0;
        if (!count(
                countValue,
                limits_.maximumArgumentsPerCall,
                "argument count")) {
            return false;
        }
        arguments.clear();
        arguments.reserve(countValue);
        for (std::size_t index = 0; index < countValue; ++index) {
            GameplayValue argument;
            if (!value(argument)) return false;
            arguments.push_back(std::move(argument));
        }
        return true;
    }

    [[nodiscard]] bool finished() const noexcept { return position_ == end_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    bool truncated() {
        fail(error_, GameplayStateErrorCode::TruncatedInput,
            "gameplay state ended before the declared payload was complete");
        return false;
    }

    const std::vector<std::uint8_t>& bytes_;
    GameplayStateError& error_;
    GameplayStateCodecLimits limits_;
    std::size_t position_ = 0;
    std::size_t end_ = 0;
};

bool writeCore(
    Writer& writer,
    const DeterministicGameplayRuntime::State& state,
    const GameplayStateCodecLimits& limits) {
    if (!writer.count(
            state.entities.slots.size(), limits.maximumEntities, "entity count")) {
        return false;
    }
    for (const auto& slot : state.entities.slots) {
        writer.u32(slot.generation);
        writer.boolean(slot.alive);
    }
    if (!writer.count(
            state.entities.freeIndices.size(),
            limits.maximumEntities,
            "free entity count")) {
        return false;
    }
    for (const auto index : state.entities.freeIndices) writer.u32(index);
    writer.u64(state.entities.aliveCount);

    writer.u32(state.clock.ticksPerSecond);
    writer.u64(state.clock.tick);
    writer.number(state.clock.accumulatorSeconds);
    writer.number(state.clock.droppedSeconds);
    writer.u64(state.random.state);
    writer.u64(state.random.increment);

    writer.u64(state.scheduler.nextId);
    if (!writer.count(
            state.scheduler.tasks.size(), limits.maximumTasks, "task count")) {
        return false;
    }
    for (const auto& task : state.scheduler.tasks) {
        writer.u64(task.id);
        writer.u64(task.dueTick);
        writer.u64(task.intervalTicks);
        writer.u32(task.remainingFires);
        writer.u64(task.payload);
    }

    writer.u64(state.events.nextSequence);
    if (!writer.count(
            state.events.events.size(), limits.maximumEvents, "event count")) {
        return false;
    }
    for (const auto& event : state.events.events) {
        writer.u64(event.dueTick);
        writer.u64(event.sequence);
        writer.u64(event.target);
        if (!writer.string(event.topic) || !writer.arguments(event.arguments)) {
            return false;
        }
    }
    return writer.withinLimit();
}

bool readCore(
    Reader& reader,
    DeterministicGameplayRuntime::State& state,
    const GameplayStateCodecLimits& limits) {
    std::size_t countValue = 0;
    if (!reader.count(countValue, limits.maximumEntities, "entity count")) {
        return false;
    }
    state.entities.slots.resize(countValue);
    for (auto& slot : state.entities.slots) {
        if (!reader.u32(slot.generation) || !reader.boolean(slot.alive)) return false;
    }
    if (!reader.count(
            countValue, limits.maximumEntities, "free entity count")) {
        return false;
    }
    state.entities.freeIndices.resize(countValue);
    for (auto& index : state.entities.freeIndices) {
        if (!reader.u32(index)) return false;
    }
    std::uint64_t aliveCount = 0;
    if (!reader.u64(aliveCount) ||
        aliveCount > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    state.entities.aliveCount = static_cast<std::size_t>(aliveCount);

    if (!reader.u32(state.clock.ticksPerSecond) ||
        !reader.u64(state.clock.tick) ||
        !reader.number(state.clock.accumulatorSeconds) ||
        !reader.number(state.clock.droppedSeconds) ||
        !reader.u64(state.random.state) ||
        !reader.u64(state.random.increment)) {
        return false;
    }

    if (!reader.u64(state.scheduler.nextId) ||
        !reader.count(countValue, limits.maximumTasks, "task count")) {
        return false;
    }
    state.scheduler.tasks.resize(countValue);
    for (auto& task : state.scheduler.tasks) {
        if (!reader.u64(task.id) ||
            !reader.u64(task.dueTick) ||
            !reader.u64(task.intervalTicks) ||
            !reader.u32(task.remainingFires) ||
            !reader.u64(task.payload)) {
            return false;
        }
    }

    if (!reader.u64(state.events.nextSequence) ||
        !reader.count(countValue, limits.maximumEvents, "event count")) {
        return false;
    }
    state.events.events.resize(countValue);
    for (auto& event : state.events.events) {
        if (!reader.u64(event.dueTick) ||
            !reader.u64(event.sequence) ||
            !reader.u64(event.target) ||
            !reader.string(event.topic) ||
            !reader.arguments(event.arguments)) {
            return false;
        }
    }
    return true;
}

bool writeHost(
    Writer& writer,
    const GameplayHost::State& state,
    const GameplayStateCodecLimits& limits) {
    if (!writeCore(writer, state.core, limits)) return false;
    writer.u64(state.nextPayloadId);
    writer.u64(state.nextSubscriptionId);
    writer.u64(state.nextSequenceId);

    if (!writer.count(state.calls.size(), limits.maximumCalls, "call count")) {
        return false;
    }
    for (const auto& call : state.calls) {
        writer.u64(call.payloadId);
        writer.u64(call.timerId);
        writer.u64(call.target);
        if (!writer.string(call.callback) || !writer.arguments(call.arguments)) {
            return false;
        }
    }

    if (!writer.count(
            state.subscriptions.size(),
            limits.maximumSubscriptions,
            "subscription count")) {
        return false;
    }
    for (const auto& subscription : state.subscriptions) {
        writer.u64(subscription.id);
        if (!writer.string(subscription.topic)) return false;
        writer.u64(subscription.target);
        if (!writer.string(subscription.callback)) return false;
        writer.u32(static_cast<std::uint32_t>(subscription.priority));
    }

    if (!writer.count(
            state.sequences.size(), limits.maximumSequences, "sequence count")) {
        return false;
    }
    for (const auto& sequence : state.sequences) {
        writer.u64(sequence.id);
        if (!writer.count(
                sequence.timers.size(), limits.maximumTasks, "sequence timer count")) {
            return false;
        }
        for (const auto timerId : sequence.timers) writer.u64(timerId);
    }
    return writer.withinLimit();
}

bool readHost(
    Reader& reader,
    GameplayHost::State& state,
    const GameplayStateCodecLimits& limits) {
    if (!readCore(reader, state.core, limits) ||
        !reader.u64(state.nextPayloadId) ||
        !reader.u64(state.nextSubscriptionId) ||
        !reader.u64(state.nextSequenceId)) {
        return false;
    }

    std::size_t countValue = 0;
    if (!reader.count(countValue, limits.maximumCalls, "call count")) return false;
    state.calls.resize(countValue);
    for (auto& call : state.calls) {
        if (!reader.u64(call.payloadId) ||
            !reader.u64(call.timerId) ||
            !reader.u64(call.target) ||
            !reader.string(call.callback) ||
            !reader.arguments(call.arguments)) {
            return false;
        }
    }

    if (!reader.count(
            countValue,
            limits.maximumSubscriptions,
            "subscription count")) {
        return false;
    }
    state.subscriptions.resize(countValue);
    for (auto& subscription : state.subscriptions) {
        std::uint32_t priority = 0;
        if (!reader.u64(subscription.id) ||
            !reader.string(subscription.topic) ||
            !reader.u64(subscription.target) ||
            !reader.string(subscription.callback) ||
            !reader.u32(priority)) {
            return false;
        }
        subscription.priority = static_cast<std::int32_t>(priority);
    }

    if (!reader.count(
            countValue, limits.maximumSequences, "sequence count")) {
        return false;
    }
    state.sequences.resize(countValue);
    for (auto& sequence : state.sequences) {
        std::size_t timerCount = 0;
        if (!reader.u64(sequence.id) ||
            !reader.count(
                timerCount, limits.maximumTasks, "sequence timer count")) {
            return false;
        }
        sequence.timers.resize(timerCount);
        for (auto& timerId : sequence.timers) {
            if (!reader.u64(timerId)) return false;
        }
    }
    return true;
}

void appendHeader(
    std::vector<std::uint8_t>& output,
    std::uint64_t stateHash,
    std::size_t payloadSize) {
    output.insert(output.end(), std::begin(Magic), std::end(Magic));
    const auto appendU32 = [&output](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            output.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    };
    const auto appendU64 = [&output](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            output.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    };
    appendU32(kGameplayStateCodecVersion);
    appendU64(stateHash);
    appendU64(payloadSize);
}

} // namespace

std::vector<std::uint8_t> encodeGameplayHostState(
    const GameplayHost::State& state,
    GameplayStateError& error,
    GameplayStateCodecLimits limits) {
    error = {};
    GameplayHost validator;
    if (!validator.restore(state)) {
        fail(error, GameplayStateErrorCode::InvalidState,
            "gameplay host state failed structural validation");
        return {};
    }

    Writer payloadWriter(error, limits);
    if (!writeHost(payloadWriter, state, limits) || error.failed()) return {};
    const auto& payload = payloadWriter.bytes();
    if (payload.size() > limits.maximumEncodedBytes -
        std::min(HeaderSize, limits.maximumEncodedBytes)) {
        fail(error, GameplayStateErrorCode::LimitExceeded,
            "encoded gameplay state exceeds the byte limit");
        return {};
    }

    std::vector<std::uint8_t> output;
    output.reserve(HeaderSize + payload.size());
    appendHeader(output, validator.stableHash(), payload.size());
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

std::vector<std::uint8_t> encodeGameplayHostState(
    const GameplayHost& host,
    GameplayStateError& error,
    GameplayStateCodecLimits limits) {
    return encodeGameplayHostState(host.snapshot(), error, limits);
}

std::optional<GameplayHost::State> decodeGameplayHostState(
    const std::vector<std::uint8_t>& bytes,
    GameplayStateError& error,
    GameplayStateCodecLimits limits) {
    error = {};
    if (bytes.size() > limits.maximumEncodedBytes) {
        fail(error, GameplayStateErrorCode::LimitExceeded,
            "encoded gameplay state exceeds the byte limit");
        return std::nullopt;
    }
    if (bytes.size() < HeaderSize) {
        fail(error, GameplayStateErrorCode::TruncatedInput,
            "gameplay state header is truncated");
        return std::nullopt;
    }
    if (!std::equal(std::begin(Magic), std::end(Magic), bytes.begin())) {
        fail(error, GameplayStateErrorCode::InvalidMagic,
            "gameplay state magic is invalid");
        return std::nullopt;
    }

    Reader header(bytes, error, limits, 4, HeaderSize);
    std::uint32_t version = 0;
    std::uint64_t expectedHash = 0;
    std::uint64_t payloadSize = 0;
    if (!header.u32(version) || !header.u64(expectedHash) ||
        !header.u64(payloadSize)) {
        return std::nullopt;
    }
    if (version != kGameplayStateCodecVersion) {
        fail(error, GameplayStateErrorCode::UnsupportedVersion,
            "gameplay state codec version is unsupported");
        return std::nullopt;
    }
    if (payloadSize != bytes.size() - HeaderSize) {
        fail(error, GameplayStateErrorCode::MalformedInput,
            "gameplay state payload size does not match the container");
        return std::nullopt;
    }

    Reader payload(bytes, error, limits, HeaderSize, bytes.size());
    GameplayHost::State state;
    if (!readHost(payload, state, limits) || !payload.finished()) {
        if (!error.failed()) {
            fail(error, GameplayStateErrorCode::MalformedInput,
                "gameplay state contains trailing or malformed payload data");
        }
        return std::nullopt;
    }

    GameplayHost validator;
    if (!validator.restore(state)) {
        fail(error, GameplayStateErrorCode::InvalidState,
            "decoded gameplay host state failed structural validation");
        return std::nullopt;
    }
    if (validator.stableHash() != expectedHash) {
        fail(error, GameplayStateErrorCode::HashMismatch,
            "decoded gameplay host state hash does not match the container");
        return std::nullopt;
    }
    return state;
}

bool restoreGameplayHostState(
    GameplayHost& host,
    const std::vector<std::uint8_t>& bytes,
    GameplayStateError& error,
    GameplayStateCodecLimits limits) {
    const auto decoded = decodeGameplayHostState(bytes, error, limits);
    return decoded && host.restore(*decoded);
}

const char* gameplayStateErrorCodeName(
    GameplayStateErrorCode code) noexcept {
    switch (code) {
    case GameplayStateErrorCode::None: return "none";
    case GameplayStateErrorCode::InvalidState: return "invalid-state";
    case GameplayStateErrorCode::InvalidMagic: return "invalid-magic";
    case GameplayStateErrorCode::UnsupportedVersion: return "unsupported-version";
    case GameplayStateErrorCode::TruncatedInput: return "truncated-input";
    case GameplayStateErrorCode::MalformedInput: return "malformed-input";
    case GameplayStateErrorCode::LimitExceeded: return "limit-exceeded";
    case GameplayStateErrorCode::HashMismatch: return "hash-mismatch";
    }
    return "unknown";
}

} // namespace realscript::game
