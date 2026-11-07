#include "realscript/game/GameProductization.h"

#include "realscript/tooling/Json.h"

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
    Byte,
    SByte,
    Short,
    UShort,
    UInt,
    ULong,
    Float,
    Char,
    Fixed,
    Fixed16,
    Vec2,
    Vec3,
    Quat,
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
        std::holds_alternative<runtime::ByteValue>(value) ||
        std::holds_alternative<runtime::SByteValue>(value) ||
        std::holds_alternative<runtime::ShortValue>(value) ||
        std::holds_alternative<runtime::UShortValue>(value) ||
        std::holds_alternative<std::int64_t>(value) ||
        std::holds_alternative<runtime::UIntValue>(value) ||
        std::holds_alternative<runtime::LongValue>(value) ||
        std::holds_alternative<runtime::ULongValue>(value) ||
        std::holds_alternative<runtime::CharValue>(value) ||
        std::holds_alternative<runtime::EnumValue>(value) ||
        std::holds_alternative<runtime::FixedValue>(value) ||
        std::holds_alternative<runtime::Fixed16Value>(value) ||
        std::holds_alternative<runtime::Vec2Value>(value) ||
        std::holds_alternative<runtime::Vec3Value>(value) ||
        std::holds_alternative<runtime::QuatValue>(value)) {
        return true;
    }
    if (std::holds_alternative<runtime::FloatValue>(value) ||
        std::holds_alternative<double>(value)) return policy.allowDoubles;
    if (std::holds_alternative<std::string>(value)) return policy.allowStrings;
    const auto* structure = std::get_if<runtime::StructValue>(&value);
    if (!structure || !structure->valid()) return false;
    for (std::size_t i = 0; i < structure->fieldCount(); ++i) {
        const auto field = runtime::structValueFieldAt(*structure, i);
        if (!field || !allowedValue(*field, policy, depth + 1)) return false;
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
    } else if (const auto* item = std::get_if<runtime::ByteValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Byte));
        writer.writeU8(item->value);
    } else if (const auto* item = std::get_if<runtime::SByteValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::SByte));
        writer.writeU8(static_cast<std::uint8_t>(item->value));
    } else if (const auto* item = std::get_if<runtime::ShortValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Short));
        writer.writeU32(static_cast<std::uint16_t>(item->value));
    } else if (const auto* item = std::get_if<runtime::UShortValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::UShort));
        writer.writeU32(item->value);
    } else if (const auto* item = std::get_if<std::int64_t>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Int));
        writer.writeU64(static_cast<std::uint64_t>(*item));
    } else if (const auto* item = std::get_if<runtime::LongValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Long));
        writer.writeU64(static_cast<std::uint64_t>(item->value));
    } else if (const auto* item = std::get_if<runtime::UIntValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::UInt));
        writer.writeU32(item->value);
    } else if (const auto* item = std::get_if<runtime::ULongValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::ULong));
        writer.writeU64(item->value);
    } else if (const auto* item = std::get_if<runtime::FloatValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Float));
        std::uint32_t bits = 0;
        const auto normalized = item->value == 0.0f ? 0.0f : item->value;
        std::memcpy(&bits, &normalized, sizeof(bits));
        writer.writeU32(bits);
    } else if (const auto* item = std::get_if<double>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Double));
        writer.writeU64(canonicalDoubleBits(*item));
    } else if (const auto* item = std::get_if<runtime::EnumValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Enum));
        writer.writeU64(item->typeId);
        writer.writeU64(static_cast<std::uint64_t>(item->value));
    } else if (const auto* item = std::get_if<runtime::CharValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Char));
        writer.writeU32(item->value);
    } else if (const auto* item = std::get_if<runtime::FixedValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Fixed));
        writer.writeU32(static_cast<std::uint32_t>(item->raw));
    } else if (const auto* item = std::get_if<runtime::Fixed16Value>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Fixed16));
        writer.writeU64(static_cast<std::uint64_t>(item->raw));
    } else if (const auto* item = std::get_if<runtime::Vec2Value>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Vec2));
        writer.writeU32(static_cast<std::uint32_t>(item->x));
        writer.writeU32(static_cast<std::uint32_t>(item->y));
    } else if (const auto* item = std::get_if<runtime::Vec3Value>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Vec3));
        writer.writeU32(static_cast<std::uint32_t>(item->x));
        writer.writeU32(static_cast<std::uint32_t>(item->y));
        writer.writeU32(static_cast<std::uint32_t>(item->z));
    } else if (const auto* item = std::get_if<runtime::QuatValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Quat));
        writer.writeU32(static_cast<std::uint32_t>(item->x));
        writer.writeU32(static_cast<std::uint32_t>(item->y));
        writer.writeU32(static_cast<std::uint32_t>(item->z));
        writer.writeU32(static_cast<std::uint32_t>(item->w));
    } else if (const auto* item = std::get_if<runtime::StructValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Struct));
        writer.writeU64(item->typeId);
        writer.writeU32(static_cast<std::uint32_t>(item->fieldCount()));
        for (std::size_t i = 0; i < item->fieldCount(); ++i) {
            const auto field = runtime::structValueFieldAt(*item, i);
            if (!field || !writeValue(writer, *field, policy, depth + 1, error)) return false;
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
        rawTag > static_cast<std::uint8_t>(ValueTag::Quat)) {
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
    case ValueTag::Byte: {
        std::uint8_t item = 0;
        if (!reader.readU8(item)) return false;
        value = runtime::ByteValue{item};
        return true;
    }
    case ValueTag::SByte: {
        std::uint8_t item = 0;
        if (!reader.readU8(item)) return false;
        value = runtime::SByteValue{static_cast<std::int8_t>(item)};
        return true;
    }
    case ValueTag::Short:
        { std::uint32_t item = 0; if (!reader.readU32(item)) return false;
          value = runtime::ShortValue{static_cast<std::int16_t>(item)}; return true; }
    case ValueTag::UShort:
        { std::uint32_t item = 0; if (!reader.readU32(item) || item > 0xffffU) return false;
          value = runtime::UShortValue{static_cast<std::uint16_t>(item)}; return true; }
    case ValueTag::Int:
        if (!reader.readU64(first)) return false;
        value = static_cast<std::int64_t>(first);
        return true;
    case ValueTag::Long:
        if (!reader.readU64(first)) return false;
        value = runtime::LongValue{static_cast<std::int64_t>(first)};
        return true;
    case ValueTag::UInt:
        { std::uint32_t item = 0; if (!reader.readU32(item)) return false;
          value = runtime::UIntValue{item}; return true; }
    case ValueTag::ULong:
        if (!reader.readU64(first)) return false;
        value = runtime::ULongValue{first};
        return true;
    case ValueTag::Float: {
        if (!policy.allowDoubles) return false;
        std::uint32_t bits = 0;
        if (!reader.readU32(bits)) return false;
        float item = 0.0f;
        std::memcpy(&item, &bits, sizeof(item));
        value = runtime::FloatValue{item};
        return true;
    }
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
        std::vector<runtime::Value> fields(count);
        for (auto& field : fields) {
            if (!readValue(reader, field, policy, depth + 1, error)) return false;
        }
        value = runtime::makeStructValueFromFields(first, std::move(fields));
        return true;
    }
    case ValueTag::String: {
        if (!policy.allowStrings) return false;
        std::string item;
        if (!reader.readString(item)) return false;
        value = std::move(item);
        return true;
    }
    case ValueTag::Char: {
        std::uint32_t item = 0;
        if (!reader.readU32(item) || item > 0xffffU) return false;
        value = runtime::CharValue{static_cast<char16_t>(item)};
        return true;
    }
    case ValueTag::Fixed: {
        std::uint32_t raw = 0;
        if (!reader.readU32(raw)) return false;
        value = runtime::FixedValue{static_cast<std::int32_t>(raw)};
        return true;
    }
    case ValueTag::Fixed16: {
        if (!reader.readU64(first)) return false;
        value = runtime::Fixed16Value{static_cast<std::int64_t>(first)};
        return true;
    }
    case ValueTag::Vec2: {
        std::uint32_t x = 0, y = 0;
        if (!reader.readU32(x) || !reader.readU32(y)) return false;
        value = runtime::Vec2Value{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
        return true;
    }
    case ValueTag::Vec3: {
        std::uint32_t x = 0, y = 0, z = 0;
        if (!reader.readU32(x) || !reader.readU32(y) || !reader.readU32(z)) return false;
        value = runtime::Vec3Value{
            static_cast<std::int32_t>(x),
            static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(z)};
        return true;
    }
    case ValueTag::Quat: {
        std::uint32_t x = 0, y = 0, z = 0, w = 0;
        if (!reader.readU32(x) || !reader.readU32(y) || !reader.readU32(z) || !reader.readU32(w)) return false;
        value = runtime::QuatValue{
            static_cast<std::int32_t>(x),
            static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(z),
            static_cast<std::int32_t>(w)};
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

namespace {

const semantic::TypeSymbol* findProgramTypeById(
    const runtime::ProgramImage* program,
    semantic::SymbolId typeId) {
    if (!program || typeId == 0) return nullptr;
    for (const auto& module : program->modules()) {
        for (const auto& type : module.types) {
            if (type.id == typeId) return &type;
        }
    }
    return nullptr;
}

bool valueToJson(
    const ScriptRuntime& runtime,
    const SceneScriptRuntime* scene,
    const runtime::Value& value,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    tooling::Json& out,
    runtime::RuntimeError& error) {
    const auto* program = runtime.program().get();
    if (depth > policy.maximumStructDepth) {
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script state exceeds maximum struct depth");
        return false;
    }
    if (std::holds_alternative<std::monostate>(value) ||
        std::holds_alternative<runtime::NullString>(value) ||
        std::holds_alternative<runtime::NullObject>(value) ||
        std::holds_alternative<runtime::NullArray>(value)) {
        out = tooling::Json(nullptr);
        return true;
    }
    if (const auto* objRef = std::get_if<runtime::ObjectRef>(&value)) {
        if (!objRef->valid()) {
            out = tooling::Json(nullptr);
            return true;
        }
        if (objRef->kind == runtime::ObjectKind::String) {
            if (!policy.allowStrings) {
                fail(error, runtime::ErrorCode::DeterminismViolation,
                    "script state contains a string disallowed by policy");
                return false;
            }
            const auto view = runtime.heap()->stringView(*objRef);
            if (!view) {
                fail(error, runtime::ErrorCode::InvalidObjectReference,
                    "failed to read script string object");
                return false;
            }
            out = tooling::Json(std::string(*view));
            return true;
        }
        if (objRef->kind == runtime::ObjectKind::Array) {
            const auto len = runtime.heap()->arrayLength(*objRef);
            if (!len) {
                fail(error, runtime::ErrorCode::InvalidObjectReference,
                    "failed to read script array length");
                return false;
            }
            tooling::Json::Array arr;
            arr.reserve(*len);
            for (std::size_t i = 0; i < *len; ++i) {
                const auto elem = runtime.heap()->arrayGet(*objRef, i);
                if (!elem) return false;
                tooling::Json elemJson;
                if (!valueToJson(
                        runtime, scene, *elem, policy, depth + 1, elemJson, error)) {
                    return false;
                }
                arr.push_back(std::move(elemJson));
            }
            out = tooling::Json(std::move(arr));
            return true;
        }
        if (scene) {
            if (const auto loc = scene->findObjectLocation(*objRef)) {
                tooling::Json::Object refObj;
                refObj["$entity"] = static_cast<std::int64_t>(loc->entity);
                refObj["$component"] = loc->canonicalTypeName;
                out = tooling::Json(std::move(refObj));
                return true;
            }
        }
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script state contains an unresolvable object reference");
        return false;
    }
    if (!std::holds_alternative<runtime::StructValue>(value) &&
        !allowedValue(value, policy, depth)) {
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script state contains a process-local or disallowed value");
        return false;
    }
    if (const auto* boolItem = std::get_if<bool>(&value)) {
        out = tooling::Json(*boolItem);
    } else if (const auto* byteItem = std::get_if<runtime::ByteValue>(&value)) {
        out = tooling::Json(static_cast<std::int64_t>(byteItem->value));
    } else if (const auto* sbyteItem = std::get_if<runtime::SByteValue>(&value)) {
        out = tooling::Json(static_cast<std::int64_t>(sbyteItem->value));
    } else if (const auto* shortItem = std::get_if<runtime::ShortValue>(&value)) {
        out = tooling::Json(static_cast<std::int64_t>(shortItem->value));
    } else if (const auto* ushortItem = std::get_if<runtime::UShortValue>(&value)) {
        out = tooling::Json(static_cast<std::int64_t>(ushortItem->value));
    } else if (const auto* intItem = std::get_if<std::int64_t>(&value)) {
        out = tooling::Json(*intItem);
    } else if (const auto* longItem = std::get_if<runtime::LongValue>(&value)) {
        out = tooling::Json(longItem->value);
    } else if (const auto* uintItem = std::get_if<runtime::UIntValue>(&value)) {
        out = tooling::Json(static_cast<std::int64_t>(uintItem->value));
    } else if (const auto* ulongItem = std::get_if<runtime::ULongValue>(&value)) {
        out = tooling::Json(static_cast<std::int64_t>(ulongItem->value));
    } else if (const auto* floatItem = std::get_if<runtime::FloatValue>(&value)) {
        out = tooling::Json(static_cast<double>(floatItem->value));
    } else if (const auto* doubleItem = std::get_if<double>(&value)) {
        out = tooling::Json(*doubleItem);
    } else if (const auto* charItem = std::get_if<runtime::CharValue>(&value)) {
        out = tooling::Json(static_cast<std::int64_t>(charItem->value));
    } else if (const auto* enumItem = std::get_if<runtime::EnumValue>(&value)) {
        out = tooling::Json(enumItem->value);
    } else if (const auto* strItem = std::get_if<std::string>(&value)) {
        out = tooling::Json(*strItem);
    } else if (const auto structTypeId = runtime::structValueTypeId(value)) {
        const auto* structDesc = findProgramTypeById(program, *structTypeId);
        if (const auto* structItem = std::get_if<runtime::StructValue>(&value)) {
            if (!structItem->valid()) return false;
            if (structDesc && structDesc->fields.size() == structItem->fieldCount()) {
                tooling::Json::Object obj;
                for (std::size_t idx = 0; idx < structDesc->fields.size(); ++idx) {
                    const auto elem = runtime::structValueFieldAt(*structItem, idx);
                    if (!elem) return false;
                    tooling::Json fieldJson;
                    if (!valueToJson(
                            runtime,
                            scene,
                            *elem,
                            policy,
                            depth + 1,
                            fieldJson,
                            error)) {
                        return false;
                    }
                    obj[structDesc->fields[idx].name] = std::move(fieldJson);
                }
                out = tooling::Json(std::move(obj));
            } else {
                tooling::Json::Array arr;
                arr.reserve(structItem->fieldCount());
                for (std::size_t idx = 0; idx < structItem->fieldCount(); ++idx) {
                    const auto elem = runtime::structValueFieldAt(*structItem, idx);
                    if (!elem) return false;
                    tooling::Json fieldJson;
                    if (!valueToJson(runtime, scene, *elem, policy, depth + 1, fieldJson, error)) {
                        return false;
                    }
                    arr.push_back(std::move(fieldJson));
                }
                out = tooling::Json(std::move(arr));
            }
        } else if (structDesc) {
            tooling::Json::Object obj;
            for (std::size_t idx = 0; idx < structDesc->fields.size(); ++idx) {
                const auto fieldVal = runtime::loadInlineStructField(value, idx);
                if (!fieldVal) return false;
                tooling::Json fieldJson;
                if (!valueToJson(
                        runtime,
                        scene,
                        *fieldVal,
                        policy,
                        depth + 1,
                        fieldJson,
                        error)) {
                    return false;
                }
                obj[structDesc->fields[idx].name] = std::move(fieldJson);
            }
            out = tooling::Json(std::move(obj));
        } else {
            tooling::Json::Array arr;
            for (std::size_t idx = 0;; ++idx) {
                const auto fieldVal = runtime::loadInlineStructField(value, idx);
                if (!fieldVal) break;
                tooling::Json fieldJson;
                if (!valueToJson(runtime, scene, *fieldVal, policy, depth + 1, fieldJson, error)) {
                    return false;
                }
                arr.push_back(std::move(fieldJson));
            }
            out = tooling::Json(std::move(arr));
        }
    } else {
        return false;
    }
    return true;
}

bool jsonToValue(
    ScriptRuntime& runtime,
    SceneScriptRuntime* scene,
    const tooling::Json& json,
    semantic::PrimitiveType expectedType,
    const std::string& expectedTypeName,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    runtime::Value& out,
    runtime::RuntimeError& error) {
    const auto* program = runtime.program().get();
    if (depth > policy.maximumStructDepth) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "JSON script state exceeds maximum struct depth");
        return false;
    }
    const semantic::SymbolId typeId =
        semantic::isExactType(expectedType) && !expectedTypeName.empty()
        ? semantic::stableTypeId(expectedTypeName)
        : 0;

    switch (expectedType) {
    case semantic::PrimitiveType::Bool:
        if (!json.isBool()) return false;
        out = json.boolValue();
        return true;
    case semantic::PrimitiveType::Byte:
        if (!json.isNumber()) return false;
        out = runtime::ByteValue{static_cast<std::uint8_t>(json.integerValue())};
        return true;
    case semantic::PrimitiveType::SByte:
        if (!json.isNumber()) return false;
        out = runtime::SByteValue{static_cast<std::int8_t>(json.integerValue())};
        return true;
    case semantic::PrimitiveType::Short:
        if (!json.isNumber()) return false;
        out = runtime::ShortValue{static_cast<std::int16_t>(json.integerValue())};
        return true;
    case semantic::PrimitiveType::UShort:
        if (!json.isNumber()) return false;
        out = runtime::UShortValue{static_cast<std::uint16_t>(json.integerValue())};
        return true;
    case semantic::PrimitiveType::Int:
        if (!json.isNumber()) return false;
        out = json.integerValue();
        return true;
    case semantic::PrimitiveType::UInt:
        if (!json.isNumber()) return false;
        out = runtime::UIntValue{static_cast<std::uint32_t>(json.integerValue())};
        return true;
    case semantic::PrimitiveType::Long:
        if (!json.isNumber()) return false;
        out = runtime::LongValue{json.integerValue()};
        return true;
    case semantic::PrimitiveType::ULong:
        if (!json.isNumber()) return false;
        out = runtime::ULongValue{static_cast<std::uint64_t>(json.integerValue())};
        return true;
    case semantic::PrimitiveType::Float:
        if (!policy.allowDoubles || !json.isNumber()) return false;
        out = runtime::FloatValue{static_cast<float>(json.numberValue())};
        return true;
    case semantic::PrimitiveType::Double:
        if (!policy.allowDoubles || !json.isNumber()) return false;
        out = json.numberValue();
        return true;
    case semantic::PrimitiveType::Char:
        if (!json.isNumber()) return false;
        out = runtime::CharValue{static_cast<char16_t>(json.integerValue())};
        return true;
    case semantic::PrimitiveType::String:
        if (json.isNull()) {
            out = runtime::NullString{};
            return true;
        }
        if (!policy.allowStrings || !json.isString()) return false;
        out = json.stringValue();
        return true;
    case semantic::PrimitiveType::Object: {
        if (json.isNull()) {
            out = runtime::NullObject{};
            return true;
        }
        if (scene && json.isObject()) {
            const auto* entJson = json.find("$entity");
            const auto* compJson = json.find("$component");
            if (entJson && entJson->isNumber() && compJson && compJson->isString()) {
                const auto entityId = static_cast<SceneEntityId>(entJson->integerValue());
                auto* compObj = scene->getComponent(entityId, compJson->stringValue());
                if (compObj) {
                    out = compObj->value();
                    return true;
                }
            }
        }
        return false;
    }
    case semantic::PrimitiveType::Array: {
        if (json.isNull()) {
            out = runtime::NullArray{};
            return true;
        }
        if (!json.isArray()) return false;
        semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Error;
        std::string elemTypeName;
        if (!semantic::decodeArrayTypeName(expectedTypeName, elemPrim, elemTypeName)) {
            return false;
        }
        const semantic::SymbolId elemTypeId =
            semantic::isExactType(elemPrim) && !elemTypeName.empty()
            ? semantic::stableTypeId(elemTypeName)
            : 0;
        const auto& items = json.arrayValue();
        std::vector<runtime::Value> decodedElements(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!jsonToValue(
                    runtime,
                    scene,
                    items[i],
                    elemPrim,
                    elemTypeName,
                    policy,
                    depth + 1,
                    decodedElements[i],
                    error)) {
                return false;
            }
        }
        const runtime::Value defaultElem = decodedElements.empty()
            ? runtime::Value{std::int64_t{0}}
            : decodedElements.front();
        const auto arrRef = runtime.heap()->allocateTypedArray(
            typeId, elemPrim, elemTypeId, items.size(), defaultElem, &error);
        if (!arrRef) return false;
        for (std::size_t i = 0; i < decodedElements.size(); ++i) {
            if (!runtime.heap()->arraySet(*arrRef, i, decodedElements[i], &error)) {
                return false;
            }
        }
        out = *arrRef;
        return true;
    }
    case semantic::PrimitiveType::Enum: {
        if (typeId == 0) return false;
        if (json.isNumber()) {
            out = runtime::EnumValue{typeId, json.integerValue()};
            return true;
        }
        if (json.isString()) {
            const auto* enumDesc = findProgramTypeById(program, typeId);
            if (enumDesc) {
                for (const auto& member : enumDesc->enumMembers) {
                    if (member.name == json.stringValue()) {
                        out = runtime::EnumValue{typeId, member.value};
                        return true;
                    }
                }
            }
        }
        return false;
    }
    case semantic::PrimitiveType::Struct: {
        const auto* structDesc = findProgramTypeById(program, typeId);
        if (!structDesc || structDesc->kind != semantic::TypeKind::Struct) return false;
        auto storage = std::make_shared<runtime::StructStorage>();
        storage->fields.resize(structDesc->fields.size());
        bool decoded = false;
        if (json.isObject()) {
            decoded = true;
            for (std::size_t idx = 0; idx < structDesc->fields.size(); ++idx) {
                const auto& fDesc = structDesc->fields[idx];
                const auto* fJson = json.find(fDesc.name);
                if (!fJson || !jsonToValue(
                        runtime,
                        scene,
                        *fJson,
                        fDesc.type,
                        fDesc.typeName,
                        policy,
                        depth + 1,
                        storage->fields[idx],
                        error)) {
                    return false;
                }
            }
        } else if (json.isArray() && json.arrayValue().size() == structDesc->fields.size()) {
            decoded = true;
            for (std::size_t idx = 0; idx < structDesc->fields.size(); ++idx) {
                const auto& fDesc = structDesc->fields[idx];
                if (!jsonToValue(
                        runtime,
                        scene,
                        json.arrayValue()[idx],
                        fDesc.type,
                        fDesc.typeName,
                        policy,
                        depth + 1,
                        storage->fields[idx],
                        error)) {
                    return false;
                }
            }
        }
        if (!decoded) return false;
        if (auto inlineVal = runtime::makeDefaultInlineStructValue(typeId)) {
            for (std::size_t idx = 0; idx < storage->fields.size(); ++idx) {
                auto updated = runtime::storeInlineStructField(*inlineVal, idx, storage->fields[idx]);
                if (!updated) return false;
                inlineVal = std::move(updated);
            }
            out = std::move(*inlineVal);
            return true;
        }
        out = runtime::makeStructValueFromFields(typeId, std::move(storage->fields));
        return true;
    }
    default:
        return false;
    }
}

} // namespace

std::string exportScriptObjectJson(
    const ScriptRuntime& runtime,
    const ScriptObject& object,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy,
    const SceneScriptRuntime* scene) {
    error = {};
    if (!object.valid()) {
        fail(error, runtime::ErrorCode::InvalidObjectReference,
            "cannot export JSON for an invalid script object");
        return {};
    }
    const auto& fields = object.type().descriptor.fields;
    if (fields.size() > policy.maximumFields) {
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script object has too many fields for the state policy");
        return {};
    }

    tooling::Json::Object fieldsObj;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto& field = fields[index];
        if (field.synthetic || field.staticField || field.constField ||
            (!field.name.empty() && field.name.front() == '$')) {
            continue;
        }
        if (field.name == "__native" &&
            field.type == semantic::PrimitiveType::Handle) {
            continue;
        }
        auto value = runtime.heap()->fieldGet(object.reference(), index);
        if (!value) {
            fail(error, runtime::ErrorCode::InvalidObjectReference,
                "failed to read script field '" + field.name + "'");
            return {};
        }
        tooling::Json fieldJson;
        if (!valueToJson(
                runtime, scene, *value, policy, 0, fieldJson, error)) {
            return {};
        }
        fieldsObj[field.name] = std::move(fieldJson);
    }

    tooling::Json::Object root;
    root["version"] = static_cast<std::int64_t>(kScriptObjectStateVersion);
    root["type"] = object.type().canonicalName();
    root["fields"] = tooling::Json(std::move(fieldsObj));
    return tooling::Json(std::move(root)).dump();
}

bool importScriptObjectJson(
    ScriptRuntime& runtime,
    ScriptObject& object,
    const std::string& jsonText,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy,
    SceneScriptRuntime* scene) {
    error = {};
    if (!object.valid()) {
        fail(error, runtime::ErrorCode::InvalidObjectReference,
            "cannot import JSON into an invalid script object");
        return false;
    }
    std::string parseError;
    const auto parsed = tooling::Json::parse(jsonText, parseError);
    if (!parsed || !parsed->isObject()) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "malformed script object JSON: " + parseError);
        return false;
    }
    const auto* versionJson = parsed->find("version");
    const auto* typeJson = parsed->find("type");
    const auto* fieldsJson = parsed->find("fields");
    if (!versionJson || !versionJson->isNumber() ||
        versionJson->integerValue() != static_cast<std::int64_t>(kScriptObjectStateVersion) ||
        !typeJson || !typeJson->isString() ||
        typeJson->stringValue() != object.type().canonicalName() ||
        !fieldsJson || !fieldsJson->isObject()) {
        fail(error, runtime::ErrorCode::TypeMismatch,
            "script object JSON header or type is incompatible with target object");
        return false;
    }

    const auto& descriptors = object.type().descriptor.fields;
    if (descriptors.size() > policy.maximumFields) {
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script object has too many fields for the state policy");
        return false;
    }

    std::vector<std::pair<std::string, runtime::Value>> decodedFields;
    decodedFields.reserve(descriptors.size());
    for (const auto& desc : descriptors) {
        if (desc.synthetic || desc.staticField || desc.constField ||
            (!desc.name.empty() && desc.name.front() == '$')) {
            continue;
        }
        if (desc.name == "__native" &&
            desc.type == semantic::PrimitiveType::Handle) {
            continue;
        }
        const auto* fieldJson = fieldsJson->find(desc.name);
        if (!fieldJson) {
            continue;
        }
        runtime::Value decodedValue;
        if (!jsonToValue(
                runtime,
                scene,
                *fieldJson,
                desc.type,
                desc.typeName,
                policy,
                0,
                decodedValue,
                error)) {
            if (error.code == runtime::ErrorCode::None) {
                fail(error, runtime::ErrorCode::TypeMismatch,
                    "script object JSON field '" + desc.name + "' type mismatch");
            }
            return false;
        }
        decodedFields.push_back({desc.name, std::move(decodedValue)});
    }

    for (const auto& entry : decodedFields) {
        if (!runtime.setMember(object, entry.first, entry.second, error)) {
            return false;
        }
    }
    return true;
}

std::string exportSceneJson(
    const SceneScriptRuntime& scene,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy) {
    error = {};
    const auto& runtime = scene.runtime();
    tooling::Json::Array entitiesArr;

    const auto encodeVec3Raw = [](const Vec3& v) {
        tooling::Json::Array arr;
        arr.push_back(tooling::Json(static_cast<std::int64_t>(v.x.raw)));
        arr.push_back(tooling::Json(static_cast<std::int64_t>(v.y.raw)));
        arr.push_back(tooling::Json(static_cast<std::int64_t>(v.z.raw)));
        return tooling::Json(std::move(arr));
    };
    const auto encodeQuatRaw = [](const Quat& q) {
        tooling::Json::Array arr;
        arr.push_back(tooling::Json(static_cast<std::int64_t>(q.x.raw)));
        arr.push_back(tooling::Json(static_cast<std::int64_t>(q.y.raw)));
        arr.push_back(tooling::Json(static_cast<std::int64_t>(q.z.raw)));
        arr.push_back(tooling::Json(static_cast<std::int64_t>(q.w.raw)));
        return tooling::Json(std::move(arr));
    };

    for (const auto entityId : scene.entities()) {
        tooling::Json::Object entObj;
        entObj["id"] = static_cast<std::int64_t>(entityId);
        entObj["activeSelf"] = scene.isEntityActiveSelf(entityId);
        entObj["parent"] = static_cast<std::int64_t>(scene.getParent(entityId));

        const Transform3D localTf = scene.getLocalTransform(entityId);
        tooling::Json::Object tfObj;
        tfObj["positionRaw"] = encodeVec3Raw(localTf.position);
        tfObj["rotationRaw"] = encodeQuatRaw(localTf.rotation);
        tfObj["scaleRaw"] = encodeVec3Raw(localTf.scale);
        entObj["localTransform"] = tooling::Json(std::move(tfObj));

        if (const auto entName = scene.getEntityName(entityId); !entName.empty()) {
            entObj["name"] = entName;
        }
        if (const auto groups = scene.getEntityGroups(entityId); !groups.empty()) {
            tooling::Json::Array grpArr;
            for (const auto& g : groups) {
                grpArr.push_back(tooling::Json(g));
            }
            entObj["groups"] = tooling::Json(std::move(grpArr));
        }
        if (const auto col = scene.getEntityCollider2D(entityId)) {
            tooling::Json::Object colObj;
            colObj["halfExtentsRaw"] = tooling::Json(tooling::Json::Array{
                tooling::Json(static_cast<std::int64_t>(col->halfExtents.x.raw)),
                tooling::Json(static_cast<std::int64_t>(col->halfExtents.y.raw)),
            });
            colObj["radiusRaw"] = static_cast<std::int64_t>(col->radius.raw);
            colObj["isCircle"] = col->isCircle;
            colObj["isTrigger"] = col->isTrigger;
            colObj["layerMask"] = static_cast<std::int64_t>(col->layerMask);
            colObj["collisionLayer"] = static_cast<std::int64_t>(col->collisionLayer);
            colObj["collisionMask"] = static_cast<std::int64_t>(col->collisionMask);
            entObj["collider2D"] = tooling::Json(std::move(colObj));
        }

        tooling::Json::Array compsArr;
        for (const auto* compObj : scene.getComponents(entityId)) {
            if (!compObj || !compObj->valid()) continue;
            const auto& fields = compObj->type().descriptor.fields;
            if (fields.size() > policy.maximumFields) {
                fail(error, runtime::ErrorCode::DeterminismViolation,
                    "script object has too many fields for the state policy");
                return {};
            }
            tooling::Json::Object fieldsObj;
            for (std::size_t index = 0; index < fields.size(); ++index) {
                const auto& field = fields[index];
                if (field.synthetic || field.staticField || field.constField ||
                    (!field.name.empty() && field.name.front() == '$')) {
                    continue;
                }
                if (field.name == "__native" &&
                    field.type == semantic::PrimitiveType::Handle) {
                    continue;
                }
                auto value = runtime.heap()->fieldGet(compObj->reference(), index);
                if (!value) {
                    fail(error, runtime::ErrorCode::InvalidObjectReference,
                        "failed to read script field '" + field.name + "'");
                    return {};
                }
                tooling::Json fieldJson;
                if (!valueToJson(
                        runtime, &scene, *value, policy, 0, fieldJson, error)) {
                    return {};
                }
                fieldsObj[field.name] = std::move(fieldJson);
            }
            tooling::Json::Object compJson;
            compJson["type"] = compObj->type().canonicalName();
            compJson["enabled"] = scene.isComponentEnabled(
                entityId, compObj->type().canonicalName());
            compJson["fields"] = tooling::Json(std::move(fieldsObj));
            compsArr.push_back(tooling::Json(std::move(compJson)));
        }
        entObj["components"] = tooling::Json(std::move(compsArr));
        entitiesArr.push_back(tooling::Json(std::move(entObj)));
    }

    tooling::Json::Array signalsArr;
    for (const auto& conn : scene.signalConnections()) {
        tooling::Json::Object sigObj;
        sigObj["sourceEntity"] = static_cast<std::int64_t>(conn.sourceEntity);
        sigObj["sourceComponent"] = conn.sourceComponent;
        sigObj["signalName"] = conn.signalName;
        sigObj["targetEntity"] = static_cast<std::int64_t>(conn.targetEntity);
        sigObj["targetComponent"] = conn.targetComponent;
        sigObj["targetMethod"] = conn.targetMethod;
        signalsArr.push_back(tooling::Json(std::move(sigObj)));
    }

    tooling::Json::Array overlapsArr;
    const auto& wasTriggerMap = scene.activeOverlapWasTrigger2D();
    for (const auto& pair : scene.activeOverlaps2D()) {
        tooling::Json::Object ovObj;
        ovObj["entityA"] = static_cast<std::int64_t>(pair.first);
        ovObj["entityB"] = static_cast<std::int64_t>(pair.second);
        const auto trigIt = wasTriggerMap.find(pair);
        ovObj["wasTrigger"] = (trigIt != wasTriggerMap.end()) ? trigIt->second : false;
        overlapsArr.push_back(tooling::Json(std::move(ovObj)));
    }

    tooling::Json::Object inputObj;
    {
        tooling::Json::Object curActObj;
        for (const auto& [k, v] : scene.currentInputActions()) {
            curActObj[k] = v;
        }
        tooling::Json::Object prevActObj;
        for (const auto& [k, v] : scene.previousInputActions()) {
            prevActObj[k] = v;
        }
        tooling::Json::Object axesObj;
        for (const auto& [k, v] : scene.inputAxes2D()) {
            axesObj[k] = tooling::Json(tooling::Json::Array{
                tooling::Json(static_cast<std::int64_t>(v.x.raw)),
                tooling::Json(static_cast<std::int64_t>(v.y.raw)),
            });
        }
        inputObj["currentActions"] = tooling::Json(std::move(curActObj));
        inputObj["previousActions"] = tooling::Json(std::move(prevActObj));
        inputObj["axes2D"] = tooling::Json(std::move(axesObj));
    }

    tooling::Json::Object root;
    root["version"] = static_cast<std::int64_t>(kScriptObjectStateVersion);
    root["entities"] = tooling::Json(std::move(entitiesArr));
    root["signalConnections"] = tooling::Json(std::move(signalsArr));
    root["activeOverlaps2D"] = tooling::Json(std::move(overlapsArr));
    root["inputSnapshot"] = tooling::Json(std::move(inputObj));
    return tooling::Json(std::move(root)).dump();
}

bool importSceneJson(
    SceneScriptRuntime& scene,
    const std::string& jsonText,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy) {
    error = {};
    std::string parseError;
    const auto parsed = tooling::Json::parse(jsonText, parseError);
    if (!parsed || !parsed->isObject()) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "malformed scene JSON: " + parseError);
        return false;
    }
    const auto* versionJson = parsed->find("version");
    const auto* entitiesJson = parsed->find("entities");
    if (!versionJson || !versionJson->isNumber() ||
        versionJson->integerValue() != static_cast<std::int64_t>(kScriptObjectStateVersion) ||
        !entitiesJson || !entitiesJson->isArray()) {
        fail(error, runtime::ErrorCode::TypeMismatch,
            "scene JSON header or entities array is invalid");
        return false;
    }

    const auto decodeVec3Raw = [](const tooling::Json* j, Vec3& out) -> bool {
        if (!j || !j->isArray() || j->arrayValue().size() != 3) return false;
        const auto& a = j->arrayValue();
        if (!a[0].isNumber() || !a[1].isNumber() || !a[2].isNumber()) return false;
        out.x = Fixed::fromRaw(static_cast<std::int32_t>(a[0].integerValue()));
        out.y = Fixed::fromRaw(static_cast<std::int32_t>(a[1].integerValue()));
        out.z = Fixed::fromRaw(static_cast<std::int32_t>(a[2].integerValue()));
        return true;
    };
    const auto decodeQuatRaw = [](const tooling::Json* j, Quat& out) -> bool {
        if (!j || !j->isArray() || j->arrayValue().size() != 4) return false;
        const auto& a = j->arrayValue();
        if (!a[0].isNumber() || !a[1].isNumber() || !a[2].isNumber() || !a[3].isNumber()) {
            return false;
        }
        out.x = Fixed::fromRaw(static_cast<std::int32_t>(a[0].integerValue()));
        out.y = Fixed::fromRaw(static_cast<std::int32_t>(a[1].integerValue()));
        out.z = Fixed::fromRaw(static_cast<std::int32_t>(a[2].integerValue()));
        out.w = Fixed::fromRaw(static_cast<std::int32_t>(a[3].integerValue()));
        return true;
    };

    // Pre-validate entity entries before clearing existing scene state
    for (const auto& entItem : entitiesJson->arrayValue()) {
        if (!entItem.isObject()) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "scene JSON entity entry must be an object");
            return false;
        }
        const auto* idJson = entItem.find("id");
        if (!idJson || !idJson->isNumber()) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "scene JSON entity entry is missing numeric 'id'");
            return false;
        }
        const auto entityId = static_cast<SceneEntityId>(idJson->integerValue());
        if (entityId == SceneScriptRuntime::BroadcastTarget) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "scene JSON entity 'id' cannot be BroadcastTarget (0)");
            return false;
        }
        if (const auto* compsJson = entItem.find("components"); compsJson && compsJson->isArray()) {
            for (const auto& compItem : compsJson->arrayValue()) {
                if (!compItem.isObject()) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "scene JSON component entry must be an object");
                    return false;
                }
                const auto* typeJson = compItem.find("type");
                if (!typeJson || !typeJson->isString()) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "scene JSON component entry is missing string 'type'");
                    return false;
                }
            }
        }
    }

    // Clear existing entities and scene-wide physics/input state
    for (const auto existingId : scene.entities()) {
        (void)scene.detach(existingId);
    }
    scene.restorePhysicsOverlapState({}, {});
    scene.restoreInputSnapshot({}, {}, {});

    struct DeferredComponentFields {
        SceneEntityId entityId = 0;
        std::string typeName;
        bool enabled = true;
        const tooling::Json* fieldsJson = nullptr;
    };
    std::vector<DeferredComponentFields> deferredComponents;
    std::vector<std::pair<SceneEntityId, SceneEntityId>> deferredParents;
    std::vector<std::pair<SceneEntityId, bool>> deferredEntityActive;

    // Pass 1: Create all entities, local transforms, names/groups/colliders, and attach all components
    for (const auto& entItem : entitiesJson->arrayValue()) {
        if (!entItem.isObject()) return false;
        const auto* idJson = entItem.find("id");
        if (!idJson || !idJson->isNumber()) return false;
        const auto entityId = static_cast<SceneEntityId>(idJson->integerValue());
        if (entityId == SceneScriptRuntime::BroadcastTarget) return false;

        const auto* activeJson = entItem.find("activeSelf");
        const bool activeSelf = (!activeJson || !activeJson->isBool()) ? true : activeJson->boolValue();
        deferredEntityActive.push_back({entityId, activeSelf});

        const auto* parentJson = entItem.find("parent");
        if (parentJson && parentJson->isNumber() && parentJson->integerValue() > 0) {
            deferredParents.push_back({
                entityId, static_cast<SceneEntityId>(parentJson->integerValue())});
        }

        if (const auto* tfJson = entItem.find("localTransform"); tfJson && tfJson->isObject()) {
            Transform3D tf = Transform3D::identity();
            (void)decodeVec3Raw(tfJson->find("positionRaw"), tf.position);
            (void)decodeQuatRaw(tfJson->find("rotationRaw"), tf.rotation);
            (void)decodeVec3Raw(tfJson->find("scaleRaw"), tf.scale);
            scene.setLocalTransform(entityId, tf);
        }

        if (const auto* nameJson = entItem.find("name"); nameJson && nameJson->isString()) {
            scene.setEntityName(entityId, nameJson->stringValue());
        }
        if (const auto* grpJson = entItem.find("groups"); grpJson && grpJson->isArray()) {
            for (const auto& g : grpJson->arrayValue()) {
                if (g.isString()) {
                    scene.addEntityToGroup(entityId, g.stringValue());
                }
            }
        }
        if (const auto* colJson = entItem.find("collider2D"); colJson && colJson->isObject()) {
            SceneCollider2D col;
            if (const auto* heJson = colJson->find("halfExtentsRaw");
                heJson && heJson->isArray() && heJson->arrayValue().size() == 2) {
                const auto& a = heJson->arrayValue();
                if (a[0].isNumber() && a[1].isNumber()) {
                    col.halfExtents.x = Fixed::fromRaw(static_cast<std::int32_t>(a[0].integerValue()));
                    col.halfExtents.y = Fixed::fromRaw(static_cast<std::int32_t>(a[1].integerValue()));
                }
            }
            if (const auto* radJson = colJson->find("radiusRaw"); radJson && radJson->isNumber()) {
                col.radius = Fixed::fromRaw(static_cast<std::int32_t>(radJson->integerValue()));
            }
            if (const auto* icJson = colJson->find("isCircle"); icJson && icJson->isBool()) {
                col.isCircle = icJson->boolValue();
            }
            if (const auto* itJson = colJson->find("isTrigger"); itJson && itJson->isBool()) {
                col.isTrigger = itJson->boolValue();
            }
            if (const auto* lmJson = colJson->find("layerMask"); lmJson && lmJson->isNumber()) {
                col.layerMask = static_cast<std::uint32_t>(lmJson->integerValue());
            }
            if (const auto* clJson = colJson->find("collisionLayer"); clJson && clJson->isNumber()) {
                col.collisionLayer = static_cast<std::uint32_t>(clJson->integerValue());
            }
            if (const auto* cmJson = colJson->find("collisionMask"); cmJson && cmJson->isNumber()) {
                col.collisionMask = static_cast<std::uint32_t>(cmJson->integerValue());
            }
            scene.setEntityCollider2D(entityId, col);
        }

        if (const auto* compsJson = entItem.find("components"); compsJson && compsJson->isArray()) {
            for (const auto& compItem : compsJson->arrayValue()) {
                if (!compItem.isObject()) return false;
                const auto* typeJson = compItem.find("type");
                if (!typeJson || !typeJson->isString()) return false;
                const auto* enabledJson = compItem.find("enabled");
                const bool compEnabled = (!enabledJson || !enabledJson->isBool())
                    ? true
                    : enabledJson->boolValue();
                SceneScriptOptions opts;
                opts.enabled = compEnabled;
                if (!scene.attachComponent(entityId, typeJson->stringValue(), std::move(opts))) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "failed to attach component '" + typeJson->stringValue() + "'");
                    return false;
                }
                deferredComponents.push_back(DeferredComponentFields{
                    entityId,
                    typeJson->stringValue(),
                    compEnabled,
                    compItem.find("fields"),
                });
            }
        }
    }

    // Pass 2: Wire hierarchy parents
    for (const auto& [childId, parentId] : deferredParents) {
        if (!scene.setParent(childId, parentId)) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "invalid parent hierarchy in scene JSON");
            return false;
        }
    }

    // Pass 3: Populate component fields (including cross-entity ObjectRefs)
    auto& runtime = scene.runtime();
    for (const auto& item : deferredComponents) {
        if (!item.fieldsJson || !item.fieldsJson->isObject()) continue;
        auto* compObj = scene.getComponent(item.entityId, item.typeName);
        if (!compObj) return false;
        for (const auto& desc : compObj->type().descriptor.fields) {
            if (desc.synthetic || desc.staticField || desc.constField ||
                (!desc.name.empty() && desc.name.front() == '$')) {
                continue;
            }
            if (desc.name == "__native" &&
                desc.type == semantic::PrimitiveType::Handle) {
                continue;
            }
            const auto* fieldJson = item.fieldsJson->find(desc.name);
            if (!fieldJson) continue;
            runtime::Value decodedValue;
            if (!jsonToValue(
                    runtime,
                    &scene,
                    *fieldJson,
                    desc.type,
                    desc.typeName,
                    policy,
                    0,
                    decodedValue,
                    error)) {
                if (error.code == runtime::ErrorCode::None) {
                    fail(error, runtime::ErrorCode::TypeMismatch,
                        "scene JSON field '" + desc.name + "' type mismatch");
                }
                return false;
            }
            if (!runtime.setMember(*compObj, desc.name, decodedValue, error)) {
                return false;
            }
        }
    }

    // Pass 4: Restore wired signals
    if (const auto* sigsJson = parsed->find("signalConnections"); sigsJson && sigsJson->isArray()) {
        for (const auto& sigItem : sigsJson->arrayValue()) {
            if (!sigItem.isObject()) return false;
            const auto* srcEnt = sigItem.find("sourceEntity");
            const auto* srcComp = sigItem.find("sourceComponent");
            const auto* sigName = sigItem.find("signalName");
            const auto* dstEnt = sigItem.find("targetEntity");
            const auto* dstComp = sigItem.find("targetComponent");
            const auto* dstMeth = sigItem.find("targetMethod");
            if (!srcEnt || !srcEnt->isNumber() || !srcComp || !srcComp->isString() ||
                !sigName || !sigName->isString() || !dstEnt || !dstEnt->isNumber() ||
                !dstComp || !dstComp->isString() || !dstMeth || !dstMeth->isString()) {
                return false;
            }
            if (!scene.connectSignal(
                    static_cast<SceneEntityId>(srcEnt->integerValue()),
                    srcComp->stringValue(),
                    sigName->stringValue(),
                    static_cast<SceneEntityId>(dstEnt->integerValue()),
                    dstComp->stringValue(),
                    dstMeth->stringValue())) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "failed to restore signal connection '" + sigName->stringValue() + "'");
                return false;
            }
        }
    }

    // Pass 5: Restore entity-level activeSelf states
    for (const auto& [entId, activeSelf] : deferredEntityActive) {
        if (!activeSelf) {
            (void)scene.setEntityActive(entId, false);
        }
    }

    // Pass 6: Restore active 2D physics overlaps and deterministic input snapshot
    if (const auto* ovsJson = parsed->find("activeOverlaps2D"); ovsJson && ovsJson->isArray()) {
        std::set<std::pair<SceneEntityId, SceneEntityId>> activeOverlaps;
        std::map<std::pair<SceneEntityId, SceneEntityId>, bool> wasTriggerMap;
        for (const auto& ovItem : ovsJson->arrayValue()) {
            if (!ovItem.isObject()) continue;
            const auto* ea = ovItem.find("entityA");
            const auto* eb = ovItem.find("entityB");
            if (!ea || !ea->isNumber() || !eb || !eb->isNumber()) continue;
            auto a = static_cast<SceneEntityId>(ea->integerValue());
            auto b = static_cast<SceneEntityId>(eb->integerValue());
            if (a > b) std::swap(a, b);
            const auto pair = std::make_pair(a, b);
            activeOverlaps.insert(pair);
            const auto* wt = ovItem.find("wasTrigger");
            wasTriggerMap[pair] = (wt && wt->isBool()) ? wt->boolValue() : false;
        }
        scene.restorePhysicsOverlapState(std::move(activeOverlaps), std::move(wasTriggerMap));
    }

    if (const auto* inpJson = parsed->find("inputSnapshot"); inpJson && inpJson->isObject()) {
        std::map<std::string, bool> curActions;
        std::map<std::string, bool> prevActions;
        std::map<std::string, Vec2> axes2D;
        if (const auto* curJson = inpJson->find("currentActions"); curJson && curJson->isObject()) {
            for (const auto& [k, v] : curJson->objectValue()) {
                if (v.isBool()) curActions[k] = v.boolValue();
            }
        }
        if (const auto* prevJson = inpJson->find("previousActions"); prevJson && prevJson->isObject()) {
            for (const auto& [k, v] : prevJson->objectValue()) {
                if (v.isBool()) prevActions[k] = v.boolValue();
            }
        }
        if (const auto* axesJson = inpJson->find("axes2D"); axesJson && axesJson->isObject()) {
            for (const auto& [k, v] : axesJson->objectValue()) {
                if (v.isArray() && v.arrayValue().size() == 2 &&
                    v.arrayValue()[0].isNumber() && v.arrayValue()[1].isNumber()) {
                    axes2D[k] = Vec2{
                        Fixed::fromRaw(static_cast<std::int32_t>(v.arrayValue()[0].integerValue())),
                        Fixed::fromRaw(static_cast<std::int32_t>(v.arrayValue()[1].integerValue())),
                    };
                }
            }
        }
        scene.restoreInputSnapshot(
            std::move(curActions), std::move(prevActions), std::move(axes2D));
    }

    return true;
}

} // namespace realscript::game
