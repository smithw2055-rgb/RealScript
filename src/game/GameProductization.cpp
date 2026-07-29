#include "realscript/game/GameProductization.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <tuple>
#include <utility>

namespace realscript::game {
namespace {

constexpr std::uint64_t FnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;
constexpr std::uint32_t StateMagic = 0x31535352u; // RSS1

enum class ValueTag : std::uint8_t {
    Void,
    NullString,
    Bool,
    Int,
    Long,
    Double,
    Enum,
    Struct,
    String,
};

void fail(runtime::RuntimeError& error, runtime::ErrorCode code, std::string message) {
    error.code = code;
    error.message = std::move(message);
}

void hashBytes(std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= FnvPrime;
    }
}

void hashU64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        const auto byte = static_cast<std::uint8_t>((value >> shift) & 0xffu);
        hashBytes(hash, &byte, 1);
    }
}

void hashString(std::uint64_t& hash, const std::string& value) noexcept {
    hashU64(hash, static_cast<std::uint64_t>(value.size()));
    hashBytes(hash, value.data(), value.size());
}

bool allowedValue(
    const runtime::Value& value,
    const ScriptStatePolicy& policy,
    std::size_t depth) {
    if (depth > policy.maximumStructDepth) return false;
    if (std::holds_alternative<std::monostate>(value) ||
        std::holds_alternative<runtime::NullString>(value) ||
        std::holds_alternative<bool>(value) ||
        std::holds_alternative<std::int64_t>(value) ||
        std::holds_alternative<runtime::LongValue>(value) ||
        std::holds_alternative<runtime::EnumValue>(value)) {
        return true;
    }
    if (std::holds_alternative<double>(value)) return policy.allowDoubles;
    if (std::holds_alternative<std::string>(value)) return policy.allowStrings;
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure || !structure->storage) return false;
    for (const auto& field : structure->storage->fields) {
        if (!allowedValue(field, policy, depth + 1)) return false;
    }
    return true;
}

class Writer {
public:
    void writeU8(std::uint8_t value) { bytes_.push_back(value); }
    void writeU32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            writeU8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void writeU64(std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            writeU8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
        }
    }
    void writeString(const std::string& value) {
        writeU32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool readU8(std::uint8_t& value) {
        if (offset_ >= bytes_.size()) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool readU32(std::uint32_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            std::uint8_t byte = 0;
            if (!readU8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool readU64(std::uint64_t& value) {
        value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            std::uint8_t byte = 0;
            if (!readU8(byte)) return false;
            value |= static_cast<std::uint64_t>(byte) << shift;
        }
        return true;
    }
    bool readString(std::string& value) {
        std::uint32_t size = 0;
        if (!readU32(size) || size > bytes_.size() - offset_) return false;
        value.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    bool atEnd() const noexcept { return offset_ == bytes_.size(); }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0;
};

std::uint64_t canonicalDoubleBits(double value) noexcept {
    if (value != value) return 0x7ff8000000000000ULL;
    if (value == 0.0) return 0;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool writeValue(
    Writer& writer,
    const runtime::Value& value,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    runtime::RuntimeError& error) {
    if (!allowedValue(value, policy, depth)) {
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script state contains a process-local or disallowed value");
        return false;
    }

    if (std::holds_alternative<std::monostate>(value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Void));
    } else if (std::holds_alternative<runtime::NullString>(value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::NullString));
    } else if (const auto* item = std::get_if<bool>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Bool));
        writer.writeU8(*item ? 1u : 0u);
    } else if (const auto* item = std::get_if<std::int64_t>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Int));
        writer.writeU64(static_cast<std::uint64_t>(*item));
    } else if (const auto* item = std::get_if<runtime::LongValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Long));
        writer.writeU64(static_cast<std::uint64_t>(item->value));
    } else if (const auto* item = std::get_if<double>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Double));
        writer.writeU64(canonicalDoubleBits(*item));
    } else if (const auto* item = std::get_if<runtime::EnumValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Enum));
        writer.writeU64(item->typeId);
        writer.writeU64(static_cast<std::uint64_t>(item->value));
    } else if (const auto* item = std::get_if<runtime::StructValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Struct));
        writer.writeU64(item->typeId);
        writer.writeU32(static_cast<std::uint32_t>(item->storage->fields.size()));
        for (const auto& field : item->storage->fields) {
            if (!writeValue(writer, field, policy, depth + 1, error)) return false;
        }
    } else if (const auto* item = std::get_if<std::string>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::String));
        writer.writeString(*item);
    }
    return true;
}

bool readValue(
    Reader& reader,
    runtime::Value& value,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    runtime::RuntimeError& error) {
    if (depth > policy.maximumStructDepth) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "script state exceeds the maximum struct depth");
        return false;
    }

    std::uint8_t rawTag = 0;
    if (!reader.readU8(rawTag) ||
        rawTag > static_cast<std::uint8_t>(ValueTag::String)) {
        return false;
    }
    const auto tag = static_cast<ValueTag>(rawTag);
    std::uint64_t first = 0;
    std::uint64_t second = 0;

    switch (tag) {
    case ValueTag::Void:
        value = std::monostate{};
        return true;
    case ValueTag::NullString:
        value = runtime::NullString{};
        return true;
    case ValueTag::Bool: {
        std::uint8_t item = 0;
        if (!reader.readU8(item) || item > 1) return false;
        value = item != 0;
        return true;
    }
    case ValueTag::Int:
        if (!reader.readU64(first)) return false;
        value = static_cast<std::int64_t>(first);
        return true;
    case ValueTag::Long:
        if (!reader.readU64(first)) return false;
        value = runtime::LongValue{static_cast<std::int64_t>(first)};
        return true;
    case ValueTag::Double: {
        if (!policy.allowDoubles || !reader.readU64(first)) return false;
        double item = 0.0;
        std::memcpy(&item, &first, sizeof(item));
        value = item;
        return true;
    }
    case ValueTag::Enum:
        if (!reader.readU64(first) || !reader.readU64(second) || first == 0) {
            return false;
        }
        value = runtime::EnumValue{first, static_cast<std::int64_t>(second)};
        return true;
    case ValueTag::Struct: {
        std::uint32_t count = 0;
        if (!reader.readU64(first) || first == 0 || !reader.readU32(count) ||
            count > policy.maximumFields) {
            return false;
        }
        auto storage = std::make_shared<runtime::StructStorage>();
        storage->fields.resize(count);
        for (auto& field : storage->fields) {
            if (!readValue(reader, field, policy, depth + 1, error)) return false;
        }
        value = runtime::StructValue{first, std::move(storage)};
        return true;
    }
    case ValueTag::String: {
        if (!policy.allowStrings) return false;
        std::string item;
        if (!reader.readString(item)) return false;
        value = std::move(item);
        return true;
    }
    }
    return false;
}

} // namespace

std::uint64_t stableProgramContentHash(
    const std::vector<bytecode::Module>& modules) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> encoded;
    encoded.reserve(modules.size());
    for (const auto& module : modules) {
        encoded.push_back({module.name, bytecode::encodeModule(module)});
    }
    std::sort(encoded.begin(), encoded.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.first, left.second) <
                std::tie(right.first, right.second);
        });

    std::uint64_t hash = FnvOffset;
    hashU64(hash, kSdkCompatibilityVersion);
    hashU64(hash, kGameSdkPackageVersion);
    hashU64(hash, static_cast<std::uint64_t>(encoded.size()));
    for (const auto& entry : encoded) {
        hashString(hash, entry.first);
        hashU64(hash, static_cast<std::uint64_t>(entry.second.size()));
        hashBytes(hash, entry.second.data(), entry.second.size());
    }
    return hash;
}

std::uint64_t stableGameApiHash(const GameApi& api) {
    auto sources = api.generatedSources();
    std::sort(sources.begin(), sources.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.path, left.content) <
                std::tie(right.path, right.content);
        });

    std::uint64_t hash = FnvOffset;
    hashU64(hash, kSdkCompatibilityVersion);
    hashU64(hash, static_cast<std::uint64_t>(sources.size()));
    for (const auto& source : sources) {
        hashString(hash, source.path);
        hashString(hash, source.content);
    }
    return hash;
}

GameProgramLoadResult GameProgramLoader::loadBytecodeModules(
    const std::vector<std::vector<std::uint8_t>>& encodedModules) const {
    GameProgramLoadResult result;
    if (!api_.valid()) {
        for (const auto& message : api_.errors()) {
            result.diagnostics.report("RS7100", message, {});
        }
        return result;
    }
    if (encodedModules.empty()) {
        result.diagnostics.report("RS7101", "no bytecode modules were supplied", {});
        return result;
    }

    result.modules.reserve(encodedModules.size());
    for (const auto& bytes : encodedModules) {
        bytecode::Module module;
        if (!bytecode::decodeModule(bytes, module, result.diagnostics)) return result;
        result.modules.push_back(std::move(module));
    }
    std::sort(result.modules.begin(), result.modules.end(),
        [](const auto& left, const auto& right) { return left.name < right.name; });

    runtime::RuntimeError linkError;
    auto image = runtime::ProgramImage::link(result.modules, linkError);
    if (!image) {
        result.diagnostics.report("RS7102", linkError.message, {});
        return result;
    }

    result.package.program =
        std::make_shared<runtime::ProgramImage>(std::move(*image));
    result.package.bindings = api_.bindings();
    result.package.heap = api_.heap();
    result.package.nativeHandles = api_.nativeHandles();
    result.package.programContentHash = stableProgramContentHash(result.modules);
    result.package.hostApiHash = stableGameApiHash(api_);
    return result;
}

std::uint64_t ScriptObjectState::canonicalHash() const noexcept {
    std::uint64_t hash = FnvOffset;
    hashU64(hash, version);
    hashString(hash, canonicalTypeName);
    hashU64(hash, static_cast<std::uint64_t>(fields.size()));
    for (const auto& field : fields) {
        hashString(hash, field.name);
        hashU64(hash, runtime::stableValueHash(field.value));
    }
    return hash;
}

std::optional<ScriptObjectState> snapshotScriptObject(
    const ScriptRuntime& runtime,
    const ScriptObject& object,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy) {
    if (!object.valid()) {
        fail(error, runtime::ErrorCode::InvalidObjectReference,
            "cannot snapshot an invalid script object");
        return std::nullopt;
    }
    const auto& fields = object.type().descriptor.fields;
    if (fields.size() > policy.maximumFields) {
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script object has too many fields for the state policy");
        return std::nullopt;
    }

    ScriptObjectState state;
    state.canonicalTypeName = object.type().canonicalName();
    state.fields.reserve(fields.size());
    for (const auto& field : fields) {
        auto value = runtime.heap()->fieldGet(object.reference(), field.index);
        if (!value) {
            fail(error, runtime::ErrorCode::InvalidObjectReference,
                "failed to read script field '" + field.name + "'");
            return std::nullopt;
        }
        if (!allowedValue(*value, policy, 0)) {
            fail(error, runtime::ErrorCode::DeterminismViolation,
                "script field '" + field.name +
                    "' is not allowed in persistent deterministic state");
            return std::nullopt;
        }
        state.fields.push_back({field.name, std::move(*value)});
    }
    return state;
}

bool restoreScriptObject(
    ScriptRuntime& runtime,
    ScriptObject& object,
    const ScriptObjectState& state,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy) {
    if (!object.valid() || state.version != kScriptObjectStateVersion ||
        object.type().canonicalName() != state.canonicalTypeName ||
        state.fields.size() != object.type().descriptor.fields.size() ||
        state.fields.size() > policy.maximumFields) {
        fail(error, runtime::ErrorCode::TypeMismatch,
            "script object state is incompatible with the target object layout");
        return false;
    }

    const auto& descriptors = object.type().descriptor.fields;
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        if (state.fields[index].name != descriptors[index].name ||
            !allowedValue(state.fields[index].value, policy, 0)) {
            fail(error, runtime::ErrorCode::TypeMismatch,
                "script object state field layout or value policy mismatch");
            return false;
        }
    }
    for (const auto& field : state.fields) {
        if (!runtime.setMember(object, field.name, field.value, error)) return false;
    }
    return true;
}

std::vector<std::uint8_t> encodeScriptObjectState(
    const ScriptObjectState& state,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy) {
    if (state.version != kScriptObjectStateVersion ||
        state.canonicalTypeName.empty() || state.fields.size() > policy.maximumFields) {
        fail(error, runtime::ErrorCode::InvalidArguments,
            "script object state header is invalid");
        return {};
    }

    std::set<std::string> names;
    Writer writer;
    writer.writeU32(StateMagic);
    writer.writeU32(state.version);
    writer.writeString(state.canonicalTypeName);
    writer.writeU32(static_cast<std::uint32_t>(state.fields.size()));
    for (const auto& field : state.fields) {
        if (field.name.empty() || !names.insert(field.name).second) {
            fail(error, runtime::ErrorCode::InvalidArguments,
                "script object state contains an empty or duplicate field name");
            return {};
        }
        writer.writeString(field.name);
        if (!writeValue(writer, field.value, policy, 0, error)) return {};
    }
    writer.writeU64(state.canonicalHash());
    auto bytes = writer.take();
    if (bytes.size() > policy.maximumEncodedBytes) {
        fail(error, runtime::ErrorCode::OutOfMemory,
            "encoded script object state exceeds the configured size limit");
        return {};
    }
    return bytes;
}

std::optional<ScriptObjectState> decodeScriptObjectState(
    const std::vector<std::uint8_t>& bytes,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy) {
    if (bytes.empty() || bytes.size() > policy.maximumEncodedBytes) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "script object state size is invalid");
        return std::nullopt;
    }

    Reader reader(bytes);
    ScriptObjectState state;
    std::uint32_t magic = 0;
    std::uint32_t count = 0;
    if (!reader.readU32(magic) || magic != StateMagic ||
        !reader.readU32(state.version) || state.version != kScriptObjectStateVersion ||
        !reader.readString(state.canonicalTypeName) || state.canonicalTypeName.empty() ||
        !reader.readU32(count) || count > policy.maximumFields) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "script object state header is malformed or unsupported");
        return std::nullopt;
    }

    std::set<std::string> names;
    state.fields.resize(count);
    for (auto& field : state.fields) {
        if (!reader.readString(field.name) || field.name.empty() ||
            !names.insert(field.name).second ||
            !readValue(reader, field.value, policy, 0, error)) {
            if (error.code == runtime::ErrorCode::None) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "script object state field data is malformed");
            }
            return std::nullopt;
        }
    }

    std::uint64_t storedHash = 0;
    if (!reader.readU64(storedHash) || !reader.atEnd() ||
        storedHash != state.canonicalHash()) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "script object state hash validation failed");
        return std::nullopt;
    }
    return state;
}

} // namespace realscript::game
