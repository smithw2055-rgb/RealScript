#include "realscript/game/GameProductization.h"

#include "realscript/game/GameplayMetadata.h"
#include "realscript/tooling/Json.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <tuple>
#include <utility>
#include <array>
#include <charconv>
#include <cstdio>
#include <iomanip>
#include <map>
#include <sstream>

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
    Fixed64,
    Vec2f,
    Vec3f,
    Quatf,
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
        std::holds_alternative<runtime::Fixed64Value>(value) ||
        std::holds_alternative<runtime::Vec3Value>(value) ||
        std::holds_alternative<runtime::QuatValue>(value)) {
        return true;
    }
    if (std::holds_alternative<runtime::FloatValue>(value) ||
        std::holds_alternative<double>(value) ||
        std::holds_alternative<runtime::Vec2fValue>(value) ||
        std::holds_alternative<runtime::Vec3fValue>(value) ||
        std::holds_alternative<runtime::QuatfValue>(value)) return policy.allowDoubles;
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
    } else if (runtime::EnumValue item{}; value.tryGetEnum(item)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Enum));
        writer.writeU64(item.typeId);
        writer.writeU64(static_cast<std::uint64_t>(item.value));
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
    } else if (runtime::QuatValue item{}; value.tryGetQuat(item)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Quat));
        writer.writeU32(static_cast<std::uint32_t>(item.x));
        writer.writeU32(static_cast<std::uint32_t>(item.y));
        writer.writeU32(static_cast<std::uint32_t>(item.z));
        writer.writeU32(static_cast<std::uint32_t>(item.w));
    } else if (const auto* item = std::get_if<runtime::Fixed64Value>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Fixed64));
        writer.writeU64(static_cast<std::uint64_t>(item->raw));
    } else if (const auto* item = std::get_if<runtime::Vec2fValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Vec2f));
        std::uint32_t bx = 0, by = 0;
        std::memcpy(&bx, &item->x, 4); std::memcpy(&by, &item->y, 4);
        writer.writeU32(bx); writer.writeU32(by);
    } else if (const auto* item = std::get_if<runtime::Vec3fValue>(&value)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Vec3f));
        std::uint32_t bx = 0, by = 0, bz = 0;
        std::memcpy(&bx, &item->x, 4); std::memcpy(&by, &item->y, 4); std::memcpy(&bz, &item->z, 4);
        writer.writeU32(bx); writer.writeU32(by); writer.writeU32(bz);
    } else if (runtime::QuatfValue qf{}; value.tryGetQuatf(qf)) {
        writer.writeU8(static_cast<std::uint8_t>(ValueTag::Quatf));
        std::uint32_t bx = 0, by = 0, bz = 0, bw = 0;
        std::memcpy(&bx, &qf.x, 4); std::memcpy(&by, &qf.y, 4);
        std::memcpy(&bz, &qf.z, 4); std::memcpy(&bw, &qf.w, 4);
        writer.writeU32(bx); writer.writeU32(by); writer.writeU32(bz); writer.writeU32(bw);
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
    case ValueTag::Fixed64: {
        if (!reader.readU64(first)) return false;
        value = runtime::Fixed64Value{static_cast<std::int64_t>(first)};
        return true;
    }
    case ValueTag::Vec2f: {
        if (!policy.allowDoubles) return false;
        std::uint32_t bx = 0, by = 0;
        if (!reader.readU32(bx) || !reader.readU32(by)) return false;
        runtime::Vec2fValue out{};
        std::memcpy(&out.x, &bx, 4); std::memcpy(&out.y, &by, 4);
        value = out;
        return true;
    }
    case ValueTag::Vec3f: {
        if (!policy.allowDoubles) return false;
        std::uint32_t bx = 0, by = 0, bz = 0;
        if (!reader.readU32(bx) || !reader.readU32(by) || !reader.readU32(bz)) return false;
        runtime::Vec3fValue out{};
        std::memcpy(&out.x, &bx, 4); std::memcpy(&out.y, &by, 4); std::memcpy(&out.z, &bz, 4);
        value = out;
        return true;
    }
    case ValueTag::Quatf: {
        if (!policy.allowDoubles) return false;
        std::uint32_t bx = 0, by = 0, bz = 0, bw = 0;
        if (!reader.readU32(bx) || !reader.readU32(by) || !reader.readU32(bz) || !reader.readU32(bw)) return false;
        runtime::QuatfValue out{};
        std::memcpy(&out.x, &bx, 4); std::memcpy(&out.y, &by, 4);
        std::memcpy(&out.z, &bz, 4); std::memcpy(&out.w, &bw, 4);
        value = out;
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
        result.abiCompatibility = preflightBytecodePackageHeaders(encodedModules);
        result.diagnostics.report("RS7101", "no bytecode modules were supplied", {});
        return result;
    }

    result.modules.reserve(encodedModules.size());
    for (const auto& bytes : encodedModules) {
        bytecode::Module module;
        if (!bytecode::decodeModule(bytes, module, result.diagnostics)) {
            auto headerCheck = preflightBytecodePackageHeaders(encodedModules);
            if (!headerCheck.compatible()) {
                result.abiCompatibility = std::move(headerCheck);
            }
            return result;
        }
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
    result.package.abiManifest = buildPackageAbiManifest(result.modules);
    result.abiCompatibility = verifyPackageAbiCompatibility(result.package);
    return result;
}

GameProgramLoadResult GameProgramLoader::loadBytecodeModules(
    const std::vector<std::vector<std::uint8_t>>& encodedModules,
    const AbiContractManifest& embeddedPackageManifest) const {
    GameProgramLoadResult result;
    result.abiCompatibility =
        verifyAbiCompatibility(currentAbiContractManifest(), embeddedPackageManifest);
    if (!result.abiCompatibility.canLoad()) {
        for (const auto& issue : result.abiCompatibility.issues) {
            if (issue.impact == AbiCompatibilityStatus::Incompatible) {
                result.diagnostics.report(
                    issue.diagnosticCode.empty() ? "RS7103" : issue.diagnosticCode,
                    "package ABI contract rejected: " + issue.message,
                    {});
            }
        }
        if (!result.diagnostics.hasErrors()) {
            result.diagnostics.report(
                "RS7103",
                "package ABI contract is incompatible with host runtime",
                {});
        }
        return result;
    }

    auto headerReport = preflightBytecodePackageHeaders(encodedModules);
    if (!headerReport.canLoad()) {
        result.abiCompatibility = std::move(headerReport);
        for (const auto& issue : result.abiCompatibility.issues) {
            if (issue.impact == AbiCompatibilityStatus::Incompatible) {
                result.diagnostics.report(
                    issue.diagnosticCode.empty() ? "RS7104" : issue.diagnosticCode,
                    issue.message,
                    {});
            }
        }
        if (!result.diagnostics.hasErrors()) {
            result.diagnostics.report(
                "RS7104",
                "package bytecode headers are incompatible with host runtime",
                {});
        }
        return result;
    }

    auto loaded = loadBytecodeModules(encodedModules);
    if (!loaded.succeeded()) {
        return loaded;
    }
    loaded.package.abiManifest = embeddedPackageManifest;
    loaded.abiCompatibility =
        verifyPackageAbiCompatibility(loaded.package, currentAbiContractManifest());
    for (const auto& issue : headerReport.issues) {
        const bool alreadyPresent = std::any_of(
            loaded.abiCompatibility.issues.begin(),
            loaded.abiCompatibility.issues.end(),
            [&](const AbiIssue& existing) {
                return existing.code == issue.code && existing.field == issue.field;
            });
        if (!alreadyPresent) {
            if (static_cast<std::uint8_t>(issue.impact) >
                static_cast<std::uint8_t>(loaded.abiCompatibility.status)) {
                loaded.abiCompatibility.status = issue.impact;
            }
            loaded.abiCompatibility.issues.push_back(issue);
        }
    }
    return loaded;
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

bool valueToJsonImpl(
    const ScriptRuntime& runtime,
    const SceneScriptRuntime* scene,
    const runtime::Value& value,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    std::unordered_set<std::uint64_t>& activeObjects,
    tooling::Json& out,
    runtime::RuntimeError& error);

bool valueToJson(
    const ScriptRuntime& runtime,
    const SceneScriptRuntime* scene,
    const runtime::Value& value,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    tooling::Json& out,
    runtime::RuntimeError& error) {
    std::unordered_set<std::uint64_t> activeObjects;
    return valueToJsonImpl(
        runtime, scene, value, policy, depth, activeObjects, out, error);
}

bool valueToJsonImpl(
    const ScriptRuntime& runtime,
    const SceneScriptRuntime* scene,
    const runtime::Value& value,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    std::unordered_set<std::uint64_t>& activeObjects,
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
        if (scene) {
            if (const auto loc = scene->findObjectLocation(*objRef)) {
                tooling::Json::Object refObj;
                refObj["$entity"] = static_cast<std::int64_t>(loc->entity);
                refObj["$component"] = loc->canonicalTypeName;
                out = tooling::Json(std::move(refObj));
                return true;
            }
        }
        const std::uint64_t objKey =
            (static_cast<std::uint64_t>(objRef->generation) << 32u) | objRef->slot;
        if (!activeObjects.insert(objKey).second) {
            // Break object graph cycles cleanly instead of recursing to maximumStructDepth
            out = tooling::Json(nullptr);
            return true;
        }
        struct ActiveObjectGuard {
            std::unordered_set<std::uint64_t>& set;
            std::uint64_t key;
            ~ActiveObjectGuard() { set.erase(key); }
        } guard{activeObjects, objKey};

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
                if (!valueToJsonImpl(
                        runtime, scene, *elem, policy, depth + 1, activeObjects, elemJson, error)) {
                    return false;
                }
                arr.push_back(std::move(elemJson));
            }
            out = tooling::Json(std::move(arr));
            return true;
        }
        if (const auto objTypeId = runtime.heap()->objectTypeId(*objRef)) {
            if (const auto* objDesc = findProgramTypeById(program, *objTypeId)) {
                tooling::Json::Object instObj;
                instObj["$objectType"] = objDesc->moduleName.empty()
                    ? objDesc->name
                    : (objDesc->moduleName + "::" + objDesc->name);
                for (std::size_t i = 0; i < objDesc->fields.size(); ++i) {
                    const auto& fld = objDesc->fields[i];
                    if (fld.staticField || fld.constField || fld.synthetic) continue;
                    const auto fVal = runtime.heap()->fieldGet(*objRef, i);
                    if (!fVal) continue;
                    tooling::Json fieldJson;
                    if (!valueToJsonImpl(
                            runtime, scene, *fVal, policy, depth + 1, activeObjects, fieldJson, error)) {
                        return false;
                    }
                    instObj[fld.name] = std::move(fieldJson);
                }
                out = tooling::Json(std::move(instObj));
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
    } else if (runtime::EnumValue enumItem{}; value.tryGetEnum(enumItem)) {
        out = tooling::Json(enumItem.value);
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
                    if (!valueToJsonImpl(
                            runtime,
                            scene,
                            *elem,
                            policy,
                            depth + 1,
                            activeObjects,
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
                    if (!valueToJsonImpl(
                            runtime, scene, *elem, policy, depth + 1, activeObjects, fieldJson, error)) {
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
                if (!valueToJsonImpl(
                        runtime,
                        scene,
                        *fieldVal,
                        policy,
                        depth + 1,
                        activeObjects,
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
                if (!valueToJsonImpl(
                        runtime, scene, *fieldVal, policy, depth + 1, activeObjects, fieldJson, error)) {
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

runtime::Value makeDefaultTypedArrayElement(
    ScriptRuntime* runtime,
    semantic::PrimitiveType elemPrim,
    semantic::SymbolId elemTid,
    std::size_t depth = 0) {
    switch (elemPrim) {
    case semantic::PrimitiveType::Bool:
        return runtime::Value{false};
    case semantic::PrimitiveType::Byte:
        return runtime::Value{runtime::ByteValue{0}};
    case semantic::PrimitiveType::SByte:
        return runtime::Value{runtime::SByteValue{0}};
    case semantic::PrimitiveType::Short:
        return runtime::Value{runtime::ShortValue{0}};
    case semantic::PrimitiveType::UShort:
        return runtime::Value{runtime::UShortValue{0}};
    case semantic::PrimitiveType::Int:
        return runtime::Value{std::int64_t{0}};
    case semantic::PrimitiveType::UInt:
        return runtime::Value{runtime::UIntValue{0}};
    case semantic::PrimitiveType::Long:
        return runtime::Value{runtime::LongValue{0}};
    case semantic::PrimitiveType::ULong:
        return runtime::Value{runtime::ULongValue{0}};
    case semantic::PrimitiveType::Float:
        return runtime::Value{runtime::FloatValue{0.0f}};
    case semantic::PrimitiveType::Double:
        return runtime::Value{0.0};
    case semantic::PrimitiveType::Char:
        return runtime::Value{runtime::CharValue{0}};
    case semantic::PrimitiveType::String:
        return runtime::Value{runtime::NullString{}};
    case semantic::PrimitiveType::Object:
        return runtime::Value{runtime::NullObject{}};
    case semantic::PrimitiveType::Array:
        return runtime::Value{runtime::NullArray{}};
    case semantic::PrimitiveType::Enum:
        return runtime::Value{runtime::EnumValue{elemTid, 0}};
    case semantic::PrimitiveType::Handle:
        return runtime::Value{runtime::NativeHandle{0, 0, elemTid, 0}};
    case semantic::PrimitiveType::Struct: {
        if (auto inl = runtime::makeDefaultInlineStructValue(elemTid)) {
            return *inl;
        }
        if (runtime && runtime->program() && depth < 16u) {
            if (const auto* stDesc = findProgramTypeById(runtime->program().get(), elemTid);
                stDesc && stDesc->kind == semantic::TypeKind::Struct) {
                std::vector<runtime::Value> fields;
                fields.reserve(stDesc->fields.size());
                for (const auto& f : stDesc->fields) {
                    if (f.staticField || f.constField) continue;
                    const semantic::SymbolId ftid =
                        semantic::isExactType(f.type) && !f.typeName.empty()
                            ? semantic::stableTypeId(f.typeName)
                            : 0;
                    fields.push_back(makeDefaultTypedArrayElement(runtime, f.type, ftid, depth + 1u));
                }
                return runtime::Value{runtime::makeStructValueFromFields(elemTid, std::move(fields))};
            }
        }
        std::vector<runtime::Value> emptyFields;
        return runtime::Value{runtime::makeStructValueFromFields(elemTid, std::move(emptyFields))};
    }
    default:
        return runtime::Value{std::int64_t{0}};
    }
}

bool jsonToValueImpl(
    ScriptRuntime& runtime,
    SceneScriptRuntime* scene,
    const tooling::Json& json,
    semantic::PrimitiveType expectedType,
    const std::string& expectedTypeName,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    std::vector<runtime::PersistentRoot>& tempRoots,
    runtime::Value& out,
    runtime::RuntimeError& error);

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
    std::vector<runtime::PersistentRoot> tempRoots;
    return jsonToValueImpl(
        runtime, scene, json, expectedType, expectedTypeName, policy, depth, tempRoots, out, error);
}

bool jsonToValueImpl(
    ScriptRuntime& runtime,
    SceneScriptRuntime* scene,
    const tooling::Json& json,
    semantic::PrimitiveType expectedType,
    const std::string& expectedTypeName,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    std::vector<runtime::PersistentRoot>& tempRoots,
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
        if (json.isObject()) {
            std::string targetType = expectedTypeName;
            if (const auto* ot = json.find("$objectType"); ot && ot->isString()) {
                targetType = ot->stringValue();
            }
            if (!targetType.empty()) {
                auto created = runtime.createObject(targetType, error);
                if (!created) {
                    if (const auto fallbackType = runtime.findType(targetType)) {
                        error = {};
                        created = runtime.allocateUninitializedObject(*fallbackType, error);
                    }
                }
                if (!created) return false;
                if (runtime.heap()) {
                    tempRoots.push_back(runtime.heap()->retain(created->reference()));
                }
                const auto& objDesc = created->type().descriptor;
                for (std::size_t i = 0; i < objDesc.fields.size(); ++i) {
                    const auto& fld = objDesc.fields[i];
                    if (fld.staticField || fld.constField || fld.synthetic) continue;
                    if (const auto* fJson = json.find(fld.name)) {
                        runtime::Value fVal{};
                        if (!jsonToValueImpl(
                                runtime, scene, *fJson, fld.type, fld.typeName,
                                policy, depth + 1, tempRoots, fVal, error)) {
                            return false;
                        }
                        if (!runtime.heap()->fieldSet(created->reference(), i, fVal, &error)) {
                            return false;
                        }
                    }
                }
                out = created->reference();
                return true;
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
            if (!jsonToValueImpl(
                    runtime,
                    scene,
                    items[i],
                    elemPrim,
                    elemTypeName,
                    policy,
                    depth + 1,
                    tempRoots,
                    decodedElements[i],
                    error)) {
                return false;
            }
        }
        const runtime::Value defaultElem = decodedElements.empty()
            ? makeDefaultTypedArrayElement(&runtime, elemPrim, elemTypeId)
            : decodedElements.front();
        const auto arrRef = runtime.heap()->allocateTypedArray(
            typeId, elemPrim, elemTypeId, items.size(), defaultElem, &error);
        if (!arrRef) return false;
        if (runtime.heap()) {
            tempRoots.push_back(runtime.heap()->retain(*arrRef));
        }
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
                if (!fJson || !jsonToValueImpl(
                        runtime,
                        scene,
                        *fJson,
                        fDesc.type,
                        fDesc.typeName,
                        policy,
                        depth + 1,
                        tempRoots,
                        storage->fields[idx],
                        error)) {
                    return false;
                }
            }
        } else if (json.isArray() && json.arrayValue().size() == structDesc->fields.size()) {
            decoded = true;
            for (std::size_t idx = 0; idx < structDesc->fields.size(); ++idx) {
                const auto& fDesc = structDesc->fields[idx];
                if (!jsonToValueImpl(
                        runtime,
                        scene,
                        json.arrayValue()[idx],
                        fDesc.type,
                        fDesc.typeName,
                        policy,
                        depth + 1,
                        tempRoots,
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
    std::vector<runtime::PersistentRoot> tempRoots;
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
        if (!jsonToValueImpl(
                runtime,
                scene,
                *fieldJson,
                desc.type,
                desc.typeName,
                policy,
                0,
                tempRoots,
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
        if (const auto col3 = scene.getEntityCollider3D(entityId)) {
            tooling::Json::Object col3Obj;
            col3Obj["halfExtentsRaw"] = encodeVec3Raw(col3->halfExtents);
            col3Obj["radiusRaw"] = static_cast<std::int64_t>(col3->radius.raw);
            col3Obj["isSphere"] = col3->isSphere;
            col3Obj["isTrigger"] = col3->isTrigger;
            col3Obj["layerMask"] = static_cast<std::int64_t>(col3->layerMask);
            col3Obj["collisionLayer"] = static_cast<std::int64_t>(col3->collisionLayer);
            col3Obj["collisionMask"] = static_cast<std::int64_t>(col3->collisionMask);
            entObj["collider3D"] = tooling::Json(std::move(col3Obj));
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
                const bool isSequenceSynthetic =
                    field.synthetic && field.name.rfind("$sequence_", 0) == 0;
                if (!isSequenceSynthetic &&
                    (field.synthetic || field.staticField || field.constField ||
                     (!field.name.empty() && field.name.front() == '$'))) {
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
            compJson["started"] = scene.isComponentStarted(
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

    tooling::Json::Array overlaps3DArr;
    const auto& wasTriggerMap3D = scene.activeOverlapWasTrigger3D();
    for (const auto& pair : scene.activeOverlaps3D()) {
        tooling::Json::Object ovObj;
        ovObj["entityA"] = static_cast<std::int64_t>(pair.first);
        ovObj["entityB"] = static_cast<std::int64_t>(pair.second);
        const auto trigIt = wasTriggerMap3D.find(pair);
        ovObj["wasTrigger"] = (trigIt != wasTriggerMap3D.end()) ? trigIt->second : false;
        overlaps3DArr.push_back(tooling::Json(std::move(ovObj)));
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

    tooling::Json::Array ecsTablesArr;
    if (runtime.heap()) {
        for (const auto& tableName : runtime.heap()->nativeEcsTableNames()) {
            const auto* entIds = runtime.heap()->nativeEcsEntityIds(tableName);
            const auto* compVals = runtime.heap()->nativeEcsComponentValues(tableName);
            if (!entIds || !compVals || entIds->size() != compVals->size() || entIds->empty()) {
                continue;
            }
            tooling::Json::Object tableObj;
            tableObj["type"] = tableName;
            tooling::Json::Array entriesArr;
            for (std::size_t i = 0; i < entIds->size(); ++i) {
                tooling::Json valJson;
                if (!valueToJson(runtime, &scene, (*compVals)[i], policy, 0, valJson, error)) {
                    return {};
                }
                tooling::Json::Object entryObj;
                entryObj["entity"] = (*entIds)[i];
                entryObj["value"] = std::move(valJson);
                entriesArr.push_back(tooling::Json(std::move(entryObj)));
            }
            tableObj["entries"] = tooling::Json(std::move(entriesArr));
            ecsTablesArr.push_back(tooling::Json(std::move(tableObj)));
        }
    }

    tooling::Json::Object root;
    root["version"] = static_cast<std::int64_t>(kScriptObjectStateVersion);
    root["entities"] = tooling::Json(std::move(entitiesArr));
    root["signalConnections"] = tooling::Json(std::move(signalsArr));
    root["activeOverlaps2D"] = tooling::Json(std::move(overlapsArr));
    root["activeOverlaps3D"] = tooling::Json(std::move(overlaps3DArr));
    root["inputSnapshot"] = tooling::Json(std::move(inputObj));
    root["nativeEcsTables"] = tooling::Json(std::move(ecsTablesArr));
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

    // Pre-validate entity entries, hierarchy, components, and signals before clearing existing scene state
    {
        std::unordered_set<SceneEntityId> preEntityIds;
        std::unordered_map<std::uint32_t, SceneEntityId> preSlotOwners;
        std::unordered_map<SceneEntityId, SceneEntityId> preParents;
        std::unordered_map<SceneEntityId, std::vector<ScriptType>> preEntityComps;

        const auto checkEntitySlot = [&](SceneEntityId eid) -> bool {
            const std::uint32_t slot = entityIdSlot(eid);
            const std::uint32_t gen = entityIdGeneration(eid);
            const auto [it, inserted] = preSlotOwners.emplace(slot, eid);
            if (!inserted && it->second != eid) {
                if (gen != 0u || entityIdGeneration(it->second) != 0u) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "scene JSON contains colliding generational entity slot handles");
                    return false;
                }
            }
            return true;
        };

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
            if (!preEntityIds.insert(entityId).second) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "scene JSON contains duplicate entity id " + std::to_string(entityId));
                return false;
            }
            if (!checkEntitySlot(entityId)) {
                return false;
            }
            if (const auto* parentJson = entItem.find("parent");
                parentJson && parentJson->isNumber() && parentJson->integerValue() > 0) {
                const auto parentId = static_cast<SceneEntityId>(parentJson->integerValue());
                if (parentId == entityId || !checkEntitySlot(parentId)) {
                    if (error.code == runtime::ErrorCode::None) {
                        fail(error, runtime::ErrorCode::InvalidProgram,
                            "invalid self-parent hierarchy in scene JSON");
                    }
                    return false;
                }
                preParents[entityId] = parentId;
            }

            auto& compList = preEntityComps[entityId];
            std::unordered_set<std::string> seenCanonOnEntity;
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
                    const auto foundType = scene.runtime().findType(typeJson->stringValue());
                    if (!foundType ||
                        foundType->descriptor.kind != semantic::TypeKind::Class ||
                        foundType->descriptor.delegateType) {
                        fail(error, runtime::ErrorCode::InvalidProgram,
                            "unknown or non-component script type '" + typeJson->stringValue() + "'");
                        return false;
                    }
                    if (!seenCanonOnEntity.insert(foundType->canonicalName()).second) {
                        fail(error, runtime::ErrorCode::InvalidProgram,
                            "duplicate component '" + foundType->canonicalName() + "' on entity");
                        return false;
                    }
                    compList.push_back(*foundType);
                }
            }
        }

        for (const auto& [childId, startParent] : preParents) {
            SceneEntityId cursor = startParent;
            std::size_t hops = 0;
            while (cursor != SceneScriptRuntime::BroadcastTarget &&
                   hops <= preParents.size() + 1u) {
                if (cursor == childId) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "cyclic parent hierarchy in scene JSON");
                    return false;
                }
                const auto pIt = preParents.find(cursor);
                cursor = (pIt == preParents.end()) ? SceneScriptRuntime::BroadcastTarget : pIt->second;
                ++hops;
            }
        }

        const auto findPreComp = [&](SceneEntityId eid, const std::string& name) -> const ScriptType* {
            const auto eIt = preEntityComps.find(eid);
            if (eIt == preEntityComps.end() || eIt->second.empty()) return nullptr;
            if (name.empty()) return &eIt->second.front();
            for (const auto& ct : eIt->second) {
                if (ct.canonicalName() == name || ct.descriptor.name == name) return &ct;
            }
            if (const auto wanted = scene.runtime().findType(name)) {
                for (const auto& ct : eIt->second) {
                    if (ct.canonicalName() == wanted->canonicalName()) return &ct;
                }
            }
            return nullptr;
        };

        if (const auto* sigsJson = parsed->find("signalConnections"); sigsJson && sigsJson->isArray()) {
            for (const auto& sigItem : sigsJson->arrayValue()) {
                if (!sigItem.isObject()) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "scene JSON signal connection entry must be an object");
                    return false;
                }
                const auto* srcEnt = sigItem.find("sourceEntity");
                const auto* srcComp = sigItem.find("sourceComponent");
                const auto* sigName = sigItem.find("signalName");
                const auto* dstEnt = sigItem.find("targetEntity");
                const auto* dstComp = sigItem.find("targetComponent");
                const auto* dstMeth = sigItem.find("targetMethod");
                if (!srcEnt || !srcEnt->isNumber() || !srcComp || !srcComp->isString() ||
                    !sigName || !sigName->isString() || !dstEnt || !dstEnt->isNumber() ||
                    !dstComp || !dstComp->isString() || !dstMeth || !dstMeth->isString() ||
                    sigName->stringValue().empty() || dstMeth->stringValue().empty()) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "malformed signal connection entry in scene JSON");
                    return false;
                }
                const auto* srcType = findPreComp(
                    static_cast<SceneEntityId>(srcEnt->integerValue()), srcComp->stringValue());
                const auto* dstType = findPreComp(
                    static_cast<SceneEntityId>(dstEnt->integerValue()), dstComp->stringValue());
                if (!srcType || !dstType) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "signal connection references missing entity or component");
                    return false;
                }
                const std::string backingFieldName = "$event_" + sigName->stringValue() + "_delegate";
                const semantic::FieldSymbol* eventField = nullptr;
                for (const auto& f : srcType->descriptor.fields) {
                    if (f.name == backingFieldName && f.type == semantic::PrimitiveType::Object) {
                        eventField = &f;
                        break;
                    }
                }
                if (!eventField) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "signal '" + sigName->stringValue() + "' not found on source component");
                    return false;
                }
                const auto delType = scene.runtime().findType(eventField->typeName);
                if (!delType || !delType->descriptor.delegateType) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "invalid signal delegate type in scene JSON");
                    return false;
                }
                std::size_t arity = 0;
                bool hasInvoke = false;
                for (const auto& m : delType->descriptor.methods) {
                    if (m.name == "Invoke") {
                        hasInvoke = true;
                        arity = m.parameters.size() > 0 ? m.parameters.size() - 1u : 0u;
                        break;
                    }
                }
                if (!hasInvoke || !scene.runtime().findMethod(*dstType, dstMeth->stringValue(), arity)) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "signal target method '" + dstMeth->stringValue() + "' not found");
                    return false;
                }
            }
        }
    }

    // Clear existing entities and scene-wide physics/input/ECS state without firing OnDisable/OnDestroy
    scene.clearForSnapshotImport();
    if (scene.runtime().heap()) {
        scene.runtime().heap()->clearNativeEcsTables();
    }

    struct DeferredComponentFields {
        SceneEntityId entityId = 0;
        std::size_t attachedCompIndex = 0;
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
        scene.registerImportedEntityHandle(entityId);

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
        if (const auto* col3Json = entItem.find("collider3D"); col3Json && col3Json->isObject()) {
            SceneCollider3D col3;
            (void)decodeVec3Raw(col3Json->find("halfExtentsRaw"), col3.halfExtents);
            if (const auto* radJson = col3Json->find("radiusRaw"); radJson && radJson->isNumber()) {
                col3.radius = Fixed::fromRaw(static_cast<std::int32_t>(radJson->integerValue()));
            }
            if (const auto* isJson = col3Json->find("isSphere"); isJson && isJson->isBool()) {
                col3.isSphere = isJson->boolValue();
            }
            if (const auto* itJson = col3Json->find("isTrigger"); itJson && itJson->isBool()) {
                col3.isTrigger = itJson->boolValue();
            }
            if (const auto* lmJson = col3Json->find("layerMask"); lmJson && lmJson->isNumber()) {
                col3.layerMask = static_cast<std::uint32_t>(lmJson->integerValue());
            }
            if (const auto* clJson = col3Json->find("collisionLayer"); clJson && clJson->isNumber()) {
                col3.collisionLayer = static_cast<std::uint32_t>(clJson->integerValue());
            }
            if (const auto* cmJson = col3Json->find("collisionMask"); cmJson && cmJson->isNumber()) {
                col3.collisionMask = static_cast<std::uint32_t>(cmJson->integerValue());
            }
            scene.setEntityCollider3D(entityId, col3);
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
                const auto* startedJson = compItem.find("started");
                const bool compStarted = (startedJson && startedJson->isBool())
                    ? startedJson->boolValue()
                    : false;
                const std::size_t compSlotIdx = scene.componentCount(entityId);
                if (!scene.restoreComponentWithoutLifecycle(
                        entityId, typeJson->stringValue(), compEnabled, compStarted)) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "failed to attach component '" + typeJson->stringValue() + "'");
                    return false;
                }
                deferredComponents.push_back(DeferredComponentFields{
                    entityId,
                    compSlotIdx,
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

    // Pass 3: Populate component fields (including cross-entity ObjectRefs and $sequence_* coroutine state)
    auto& runtime = scene.runtime();
    for (const auto& item : deferredComponents) {
        if (!item.fieldsJson || !item.fieldsJson->isObject()) continue;
        const auto compsOnEnt = scene.getComponents(item.entityId);
        auto* compObj = item.attachedCompIndex < compsOnEnt.size()
            ? compsOnEnt[item.attachedCompIndex]
            : scene.getComponent(item.entityId, item.typeName);
        if (!compObj) return false;
        for (const auto& desc : compObj->type().descriptor.fields) {
            const bool isSequenceSynthetic =
                desc.synthetic && desc.name.rfind("$sequence_", 0) == 0;
            if (!isSequenceSynthetic &&
                (desc.synthetic || desc.staticField || desc.constField ||
                 (!desc.name.empty() && desc.name.front() == '$'))) {
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
            if (isSequenceSynthetic) {
                if (!runtime.heap() ||
                    !runtime.heap()->fieldSet(
                        compObj->reference(), desc.index, std::move(decodedValue), &error)) {
                    return false;
                }
            } else if (!runtime.setMember(*compObj, desc.name, decodedValue, error)) {
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

    // Pass 5: Restore entity-level activeSelf states without firing OnDisable/OnEnable
    for (const auto& [entId, activeSelf] : deferredEntityActive) {
        scene.restoreEntityActiveWithoutLifecycle(entId, activeSelf);
    }

    // Pass 6: Restore active 2D/3D physics overlaps and deterministic input snapshot
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

    if (const auto* ovs3Json = parsed->find("activeOverlaps3D"); ovs3Json && ovs3Json->isArray()) {
        std::set<std::pair<SceneEntityId, SceneEntityId>> activeOverlaps3D;
        std::map<std::pair<SceneEntityId, SceneEntityId>, bool> wasTriggerMap3D;
        for (const auto& ovItem : ovs3Json->arrayValue()) {
            if (!ovItem.isObject()) continue;
            const auto* ea = ovItem.find("entityA");
            const auto* eb = ovItem.find("entityB");
            if (!ea || !ea->isNumber() || !eb || !eb->isNumber()) continue;
            auto a = static_cast<SceneEntityId>(ea->integerValue());
            auto b = static_cast<SceneEntityId>(eb->integerValue());
            if (a > b) std::swap(a, b);
            const auto pair = std::make_pair(a, b);
            activeOverlaps3D.insert(pair);
            const auto* wt = ovItem.find("wasTrigger");
            wasTriggerMap3D[pair] = (wt && wt->isBool()) ? wt->boolValue() : false;
        }
        scene.restorePhysicsOverlapState3D(std::move(activeOverlaps3D), std::move(wasTriggerMap3D));
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

    // Pass 7: Restore native ECS Sparse-Set component tables
    if (const auto* ecsJson = parsed->find("nativeEcsTables");
        ecsJson && ecsJson->isArray() && runtime.heap()) {
        for (const auto& tableItem : ecsJson->arrayValue()) {
            if (!tableItem.isObject()) continue;
            const auto* typeJson = tableItem.find("type");
            const auto* entriesJson = tableItem.find("entries");
            if (!typeJson || !typeJson->isString() || !entriesJson || !entriesJson->isArray()) {
                continue;
            }
            const std::string& typeName = typeJson->stringValue();
            semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Struct;
            if (const auto scriptType = runtime.findType(typeName)) {
                if (scriptType->descriptor.kind == semantic::TypeKind::Class) {
                    elemPrim = semantic::PrimitiveType::Object;
                } else if (scriptType->descriptor.kind == semantic::TypeKind::Enum) {
                    elemPrim = semantic::PrimitiveType::Enum;
                }
            }
            for (const auto& entryItem : entriesJson->arrayValue()) {
                if (!entryItem.isObject()) continue;
                const auto* entJson = entryItem.find("entity");
                const auto* valJson = entryItem.find("value");
                if (!entJson || !entJson->isNumber() || !valJson) continue;
                runtime::Value decodedVal;
                if (!jsonToValue(
                        runtime,
                        &scene,
                        *valJson,
                        elemPrim,
                        typeName,
                        policy,
                        0,
                        decodedVal,
                        error)) {
                    return false;
                }
                (void)runtime.heap()->setNativeEcsComponent(
                    typeName, entJson->integerValue(), std::move(decodedVal));
            }
        }
    }

    return true;
}

std::optional<RollbackFrameSnapshot> captureRollbackFrame(
    const SceneScriptRuntime& scene,
    const GameplayHost& host,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy) {
    error = {};
    RollbackFrameSnapshot snap;
    snap.tick = host.core().clock().tick();
    GameplayStateError hostErr;
    snap.hostStateBytes = encodeGameplayHostState(host, hostErr);
    if (hostErr.failed() || snap.hostStateBytes.empty()) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "failed to encode GameplayHost state: " + hostErr.message);
        return std::nullopt;
    }
    snap.sceneJson = exportSceneJson(scene, error, policy);
    if (error.code != runtime::ErrorCode::None || snap.sceneJson.empty()) {
        return std::nullopt;
    }
    std::uint64_t hash = FnvOffset;
    hashU64(hash, snap.tick);
    hashBytes(hash, snap.hostStateBytes.data(), snap.hostStateBytes.size());
    hashString(hash, snap.sceneJson);
    snap.frameHash = hash;
    return snap;
}

bool restoreRollbackFrame(
    SceneScriptRuntime& scene,
    GameplayHost& host,
    const RollbackFrameSnapshot& snapshot,
    runtime::RuntimeError& error,
    ScriptStatePolicy policy) {
    error = {};
    std::uint64_t expectedHash = FnvOffset;
    hashU64(expectedHash, snapshot.tick);
    hashBytes(expectedHash, snapshot.hostStateBytes.data(), snapshot.hostStateBytes.size());
    hashString(expectedHash, snapshot.sceneJson);
    if (expectedHash != snapshot.frameHash) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "rollback frame snapshot hash mismatch");
        return false;
    }
    if (!importSceneJson(scene, snapshot.sceneJson, error, policy)) {
        return false;
    }
    GameplayStateError hostErr;
    if (!restoreGameplayHostState(host, snapshot.hostStateBytes, hostErr)) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "failed to restore GameplayHost state: " + hostErr.message);
        return false;
    }
    return true;
}


// ============================================================================
// eff_3: Deterministic 128-bit / RFC-4122 UUID v5 AssetGuid & Git-Friendly Text Codec
// ============================================================================

namespace {

int parseHexNibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string_view trimAscii(std::string_view sv) noexcept {
    while (!sv.empty() &&
           (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() &&
           (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

std::string escapeTextString(std::string_view raw) {
    std::string out;
    out.reserve(raw.size() + 2u);
    out.push_back('"');
    static constexpr const char* kHex = "0123456789abcdef";
    for (const char ch : raw) {
        const auto uc = static_cast<unsigned char>(ch);
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (uc < 0x20u) {
                out += "\\x";
                out.push_back(kHex[(uc >> 4u) & 0xFu]);
                out.push_back(kHex[uc & 0xFu]);
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

bool unescapeTextString(std::string_view token, std::string& out) {
    token = trimAscii(token);
    if (token.size() < 2u || token.front() != '"' || token.back() != '"') {
        return false;
    }
    out.clear();
    out.reserve(token.size() - 2u);
    for (std::size_t i = 1u; i + 1u < token.size(); ++i) {
        const char ch = token[i];
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (i + 2u >= token.size()) return false;
        const char esc = token[++i];
        switch (esc) {
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'x': {
            if (i + 3u >= token.size()) return false;
            const int hi = parseHexNibble(token[i + 1u]);
            const int lo = parseHexNibble(token[i + 2u]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2u;
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

bool splitTopLevelPipes(std::string_view text, std::vector<std::string_view>& parts) {
    parts.clear();
    bool inQuotes = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (inQuotes) {
            if (ch == '\\') {
                if (i + 1u >= text.size()) return false;
                ++i;
            } else if (ch == '"') {
                inQuotes = false;
            }
        } else {
            if (ch == '"') {
                inQuotes = true;
            } else if (ch == '|') {
                parts.push_back(trimAscii(text.substr(start, i - start)));
                start = i + 1u;
            }
        }
    }
    if (inQuotes) return false;
    parts.push_back(trimAscii(text.substr(start)));
    return true;
}

bool parseI64(std::string_view sv, std::int64_t& out) noexcept {
    sv = trimAscii(sv);
    if (sv.empty()) return false;
    const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return res.ec == std::errc{} && res.ptr == sv.data() + sv.size();
}

bool parseU64(std::string_view sv, std::uint64_t& out) noexcept {
    sv = trimAscii(sv);
    if (sv.empty()) return false;
    int base = 10;
    if (sv.size() > 2u && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) {
        sv.remove_prefix(2u);
        base = 16;
    }
    const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), out, base);
    return res.ec == std::errc{} && res.ptr == sv.data() + sv.size();
}

bool parseBoolToken(std::string_view sv, bool& out) noexcept {
    sv = trimAscii(sv);
    if (sv == "true" || sv == "1") { out = true; return true; }
    if (sv == "false" || sv == "0") { out = false; return true; }
    return false;
}

std::string formatZeroPaddedIndex(std::size_t idx) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "%06zu", idx);
    return std::string(buf);
}

std::string resolveTypeWithGuid(
    const AssetGuidRegistry* guidRegistry,
    std::string_view guidStr,
    const std::string& fallbackTypeName) {
    if (guidRegistry) {
        if (const auto parsed = AssetGuid::parse(guidStr); parsed && parsed->valid()) {
            if (const auto resolved = guidRegistry->resolveTypeName(*parsed)) {
                return *resolved;
            }
        }
    }
    return fallbackTypeName;
}

std::string resolveAssetPathWithGuid(
    const AssetGuidRegistry* guidRegistry,
    std::string_view guidStr,
    const std::string& fallbackPath) {
    if (guidRegistry) {
        if (const auto parsed = AssetGuid::parse(guidStr); parsed && parsed->valid()) {
            if (const auto resolved = guidRegistry->resolveAssetPath(*parsed)) {
                return *resolved;
            }
        }
    }
    return fallbackPath;
}

AssetGuid guidForTypeHelper(
    const AssetGuidRegistry* guidRegistry,
    std::string_view canonicalTypeName) {
    if (canonicalTypeName.empty()) return AssetGuid::nil();
    return guidRegistry
        ? guidRegistry->guidForTypeOrDeterministic(canonicalTypeName)
        : AssetGuid::fromTypeName(canonicalTypeName);
}

AssetGuid guidForAssetHelper(
    const AssetGuidRegistry* guidRegistry,
    std::string_view assetPath) {
    if (assetPath.empty()) return AssetGuid::nil();
    return guidRegistry
        ? guidRegistry->guidForAssetPathOrDeterministic(assetPath)
        : AssetGuid::fromAssetPath(assetPath);
}

std::optional<semantic::PrimitiveType> parsePrimitiveTypeName(std::string_view name) noexcept {
    static constexpr std::pair<std::string_view, semantic::PrimitiveType> kTable[] = {
        {"error", semantic::PrimitiveType::Error},
        {"void", semantic::PrimitiveType::Void},
        {"bool", semantic::PrimitiveType::Bool},
        {"byte", semantic::PrimitiveType::Byte},
        {"sbyte", semantic::PrimitiveType::SByte},
        {"short", semantic::PrimitiveType::Short},
        {"ushort", semantic::PrimitiveType::UShort},
        {"int", semantic::PrimitiveType::Int},
        {"uint", semantic::PrimitiveType::UInt},
        {"long", semantic::PrimitiveType::Long},
        {"ulong", semantic::PrimitiveType::ULong},
        {"float", semantic::PrimitiveType::Float},
        {"double", semantic::PrimitiveType::Double},
        {"char", semantic::PrimitiveType::Char},
        {"string", semantic::PrimitiveType::String},
        {"object", semantic::PrimitiveType::Object},
        {"array", semantic::PrimitiveType::Array},
        {"enum", semantic::PrimitiveType::Enum},
        {"struct", semantic::PrimitiveType::Struct},
        {"handle", semantic::PrimitiveType::Handle},
        {"null", semantic::PrimitiveType::Null},
    };
    for (const auto& [k, v] : kTable) {
        if (k == name) return v;
    }
    return std::nullopt;
}

std::string resolveBuiltinStructNameById(semantic::SymbolId typeId) {
    static const std::pair<semantic::SymbolId, const char*> kBuiltins[] = {
        {semantic::stableTypeId("RealScript.Game::Fixed"), "RealScript.Game::Fixed"},
        {semantic::stableTypeId("Fixed"), "Fixed"},
        {semantic::stableTypeId("RealScript.Game::Fixed16"), "RealScript.Game::Fixed16"},
        {semantic::stableTypeId("Fixed16"), "Fixed16"},
        {semantic::stableTypeId("RealScript.Game::Vec2"), "RealScript.Game::Vec2"},
        {semantic::stableTypeId("Vec2"), "Vec2"},
        {semantic::stableTypeId("RealScript.Game::Vec3"), "RealScript.Game::Vec3"},
        {semantic::stableTypeId("Vec3"), "Vec3"},
        {semantic::stableTypeId("RealScript.Game::Quat"), "RealScript.Game::Quat"},
        {semantic::stableTypeId("Quat"), "Quat"},
    };
    for (const auto& [id, name] : kBuiltins) {
        if (id == typeId) return name;
    }
    return {};
}

bool memberAllowsAssetPathBinding(
    const ScriptMetadataRegistry* metadata,
    const std::string& ownerCanonicalTypeName,
    const std::string& memberName) {
    if (!metadata || ownerCanonicalTypeName.empty() || memberName.empty()) {
        return false;
    }
    return metadata->findMemberAttribute(ownerCanonicalTypeName, memberName, "AssetRef") != nullptr ||
        metadata->findMemberAttribute(ownerCanonicalTypeName, memberName, "AssetPath") != nullptr;
}

bool isSimplePrefabMemberIdentifier(std::string_view name) noexcept {
    if (name.empty()) return false;
    for (const char ch : name) {
        const auto uc = static_cast<unsigned char>(ch);
        const bool ok = (uc >= 'a' && uc <= 'z') ||
            (uc >= 'A' && uc <= 'Z') ||
            (uc >= '0' && uc <= '9') ||
            ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

std::string encodePrefabMemberKeySegment(const std::string& memberName) {
    if (isSimplePrefabMemberIdentifier(memberName)) {
        return memberName;
    }
    return escapeTextString(memberName);
}

struct TextEncodeGraphContext {
    std::unordered_map<std::uint64_t, std::string> objectRefIds;
    const ScriptMetadataRegistry* metadata = nullptr;
};

struct TextDecodeGraphContext {
    std::unordered_map<std::string, runtime::ObjectRef> objectsById;
    std::unordered_map<std::string, std::string> defKeyByRefId;
    std::unordered_set<std::string> resolvingRefIds;

    void indexDocumentDefinitions(const std::map<std::string, std::string>& lines) {
        for (const auto& [k, rawVal] : lines) {
            const std::string_view spec = trimAscii(rawVal);
            const auto colonPos = spec.find(':');
            if (colonPos == std::string_view::npos) continue;
            const std::string_view tag = spec.substr(0, colonPos);
            const std::string_view payload = spec.substr(colonPos + 1u);
            if (tag == "object") {
                std::vector<std::string_view> parts;
                if (splitTopLevelPipes(payload, parts) && parts.size() == 3u && !parts[2].empty()) {
                    defKeyByRefId.emplace(std::string(parts[2]), k);
                }
            } else if (tag == "array") {
                std::vector<std::string_view> parts;
                if (splitTopLevelPipes(payload, parts) && parts.size() == 5u && !parts[4].empty()) {
                    defKeyByRefId.emplace(std::string(parts[4]), k);
                }
            }
        }
    }
};

bool encodeValueLines(
    const ScriptRuntime* runtime,
    const SceneScriptRuntime* scene,
    const AssetGuidRegistry* guidRegistry,
    const std::string& keyPrefix,
    const runtime::Value& value,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    TextEncodeGraphContext& graphCtx,
    std::map<std::string, std::string>& outLines,
    runtime::RuntimeError& error,
    bool allowAssetPathBinding = false,
    std::string_view declaredTypeHint = {}) {
    if (depth > policy.maximumStructDepth) {
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script state exceeds maximum struct depth");
        return false;
    }

    const runtime::ProgramImage* program = runtime ? runtime->program().get() : nullptr;

    if (std::holds_alternative<std::monostate>(value)) {
        outLines[keyPrefix] = "void";
        return true;
    }
    if (std::holds_alternative<runtime::NullString>(value)) {
        outLines[keyPrefix] = "null_str";
        return true;
    }
    if (std::holds_alternative<runtime::NullObject>(value)) {
        outLines[keyPrefix] = "null_obj";
        return true;
    }
    if (std::holds_alternative<runtime::NullArray>(value)) {
        outLines[keyPrefix] = "null_arr";
        return true;
    }

    if (const auto* objRef = std::get_if<runtime::ObjectRef>(&value)) {
        if (!objRef->valid()) {
            outLines[keyPrefix] = "null_obj";
            return true;
        }
        if (!runtime || !runtime->heap()) {
            fail(error, runtime::ErrorCode::InvalidObjectReference,
                "cannot serialize managed ObjectRef without an active ScriptRuntime");
            return false;
        }
        if (objRef->kind == runtime::ObjectKind::String) {
            if (!policy.allowStrings) {
                fail(error, runtime::ErrorCode::DeterminismViolation,
                    "script state contains a string disallowed by policy");
                return false;
            }
            const auto view = runtime->heap()->stringView(*objRef);
            if (!view) {
                fail(error, runtime::ErrorCode::InvalidObjectReference,
                    "failed to read script string object");
                return false;
            }
            if (allowAssetPathBinding && guidRegistry) {
                if (const auto assetGuid = guidRegistry->findGuidForAssetPath(*view)) {
                    outLines[keyPrefix] = "asset_path:" + assetGuid->toString() + "|" +
                        escapeTextString(AssetGuid::normalizeAssetPath(*view));
                    return true;
                }
            }
            outLines[keyPrefix] = "str:" + escapeTextString(*view);
            return true;
        }
        if (scene) {
            if (const auto loc = scene->findObjectLocation(*objRef)) {
                const AssetGuid compGuid = guidForTypeHelper(guidRegistry, loc->canonicalTypeName);
                outLines[keyPrefix] = "comp_ref:" + std::to_string(loc->entity) + "|" +
                    compGuid.toString() + "|" + escapeTextString(loc->canonicalTypeName);
                return true;
            }
        }
        if (const auto assetPath = runtime->findAssetPath(*objRef)) {
            std::string assetTypeName;
            if (const auto* assetObj = runtime->findAsset(*assetPath)) {
                assetTypeName = assetObj->type().canonicalName();
            }
            const AssetGuid pathGuid = guidForAssetHelper(guidRegistry, *assetPath);
            const AssetGuid typeGuid = guidForTypeHelper(guidRegistry, assetTypeName);
            outLines[keyPrefix] = "asset_obj_ref:" + pathGuid.toString() + "|" +
                escapeTextString(AssetGuid::normalizeAssetPath(*assetPath)) + "|" +
                typeGuid.toString() + "|" + escapeTextString(assetTypeName);
            return true;
        }

        const std::uint64_t objKey =
            (static_cast<std::uint64_t>(objRef->generation) << 32u) | objRef->slot;
        const auto existingRefIt = graphCtx.objectRefIds.find(objKey);
        if (existingRefIt != graphCtx.objectRefIds.end()) {
            outLines[keyPrefix] = "obj_ref:" + existingRefIt->second;
            return true;
        }
        const std::string objRefId =
            AssetGuid::fromCanonicalName("heap_obj", keyPrefix).toString();
        graphCtx.objectRefIds.emplace(objKey, objRefId);

        if (objRef->kind == runtime::ObjectKind::Array) {
            const auto len = runtime->heap()->arrayLength(*objRef);
            if (!len) {
                fail(error, runtime::ErrorCode::InvalidObjectReference,
                    "failed to read script array length");
                return false;
            }
            std::string arrayTypeName;
            semantic::PrimitiveType elemPrim = semantic::PrimitiveType::Int;
            std::string elemTypeName;
            if (const auto arrTid = runtime->heap()->arrayTypeId(*objRef); arrTid && *arrTid != 0) {
                if (const auto* arrDesc = findProgramTypeById(program, *arrTid)) {
                    arrayTypeName = semantic::canonicalTypeName(*arrDesc);
                    (void)semantic::decodeArrayTypeName(arrayTypeName, elemPrim, elemTypeName);
                }
            }
            if (arrayTypeName.empty()) {
                if (const auto heapElemPrim = runtime->heap()->arrayElementType(*objRef)) {
                    elemPrim = *heapElemPrim;
                    if (semantic::isExactType(elemPrim)) {
                        if (const auto heapElemTid = runtime->heap()->arrayElementTypeId(*objRef);
                            heapElemTid && *heapElemTid != 0) {
                            if (const auto* elemDesc = findProgramTypeById(program, *heapElemTid)) {
                                elemTypeName = semantic::canonicalTypeName(*elemDesc);
                            } else {
                                elemTypeName = resolveBuiltinStructNameById(*heapElemTid);
                            }
                        }
                    }
                    if (!semantic::isExactType(elemPrim) || !elemTypeName.empty()) {
                        arrayTypeName = semantic::arrayTypeName(elemPrim, elemTypeName);
                    }
                }
            }
            if (arrayTypeName.empty() && !declaredTypeHint.empty()) {
                semantic::PrimitiveType hintPrim = semantic::PrimitiveType::Error;
                std::string hintElemName;
                if (semantic::decodeArrayTypeName(std::string(declaredTypeHint), hintPrim, hintElemName)) {
                    elemPrim = hintPrim;
                    elemTypeName = std::move(hintElemName);
                    arrayTypeName = semantic::arrayTypeName(elemPrim, elemTypeName);
                }
            }
            if (arrayTypeName.empty() && *len > 0) {
                if (const auto firstElem = runtime->heap()->arrayGet(*objRef, 0)) {
                    elemPrim = runtime::valueType(*firstElem);
                }
            }
            const AssetGuid elemGuid = guidForTypeHelper(guidRegistry, elemTypeName);
            outLines[keyPrefix] = "array:" +
                std::string(semantic::primitiveTypeName(elemPrim)) + "|" +
                elemGuid.toString() + "|" +
                escapeTextString(elemTypeName) + "|" +
                std::to_string(*len) + "|" +
                objRefId;
            for (std::size_t i = 0; i < *len; ++i) {
                const auto elem = runtime->heap()->arrayGet(*objRef, i);
                if (!elem) return false;
                if (!encodeValueLines(
                        runtime,
                        scene,
                        guidRegistry,
                        keyPrefix + "[" + formatZeroPaddedIndex(i) + "]",
                        *elem,
                        policy,
                        depth + 1u,
                        graphCtx,
                        outLines,
                        error,
                        false,
                        elemTypeName)) {
                    return false;
                }
            }
            return true;
        }

        if (const auto objTid = runtime->heap()->objectTypeId(*objRef)) {
            if (const auto* objDesc = findProgramTypeById(program, *objTid)) {
                const std::string objTypeName = semantic::canonicalTypeName(*objDesc);
                const AssetGuid typeGuid = guidForTypeHelper(guidRegistry, objTypeName);
                outLines[keyPrefix] = "object:" + typeGuid.toString() + "|" +
                    escapeTextString(objTypeName) + "|" + objRefId;
                for (std::size_t i = 0; i < objDesc->fields.size(); ++i) {
                    const auto& fld = objDesc->fields[i];
                    if (fld.staticField || fld.constField || fld.synthetic) continue;
                    const auto fVal = runtime->heap()->fieldGet(*objRef, i);
                    if (!fVal) continue;
                    const bool fldAllowAssetPath =
                        memberAllowsAssetPathBinding(graphCtx.metadata, objTypeName, fld.name);
                    if (!encodeValueLines(
                            runtime,
                            scene,
                            guidRegistry,
                            keyPrefix + "." + fld.name,
                            *fVal,
                            policy,
                            depth + 1u,
                            graphCtx,
                            outLines,
                            error,
                            fldAllowAssetPath,
                            fld.typeName)) {
                        return false;
                    }
                }
                return true;
            }
        }
        fail(error, runtime::ErrorCode::DeterminismViolation,
            "script state contains an unresolvable object reference");
        return false;
    }

    if (const auto* b = std::get_if<bool>(&value)) {
        outLines[keyPrefix] = *b ? "bool:true" : "bool:false";
        return true;
    }
    if (const auto* v = std::get_if<runtime::ByteValue>(&value)) {
        outLines[keyPrefix] = "byte:" + std::to_string(v->value);
        return true;
    }
    if (const auto* v = std::get_if<runtime::SByteValue>(&value)) {
        outLines[keyPrefix] = "sbyte:" + std::to_string(static_cast<int>(v->value));
        return true;
    }
    if (const auto* v = std::get_if<runtime::ShortValue>(&value)) {
        outLines[keyPrefix] = "short:" + std::to_string(v->value);
        return true;
    }
    if (const auto* v = std::get_if<runtime::UShortValue>(&value)) {
        outLines[keyPrefix] = "ushort:" + std::to_string(v->value);
        return true;
    }
    if (const auto* v = std::get_if<std::int64_t>(&value)) {
        outLines[keyPrefix] = "int:" + std::to_string(*v);
        return true;
    }
    if (const auto* v = std::get_if<runtime::UIntValue>(&value)) {
        outLines[keyPrefix] = "uint:" + std::to_string(v->value);
        return true;
    }
    if (const auto* v = std::get_if<runtime::LongValue>(&value)) {
        outLines[keyPrefix] = "long:" + std::to_string(v->value);
        return true;
    }
    if (const auto* v = std::get_if<runtime::ULongValue>(&value)) {
        outLines[keyPrefix] = "ulong:" + std::to_string(v->value);
        return true;
    }
    if (const auto* v = std::get_if<runtime::CharValue>(&value)) {
        outLines[keyPrefix] = "char:" + std::to_string(static_cast<std::uint32_t>(v->value));
        return true;
    }
    if (const auto* v = std::get_if<runtime::FloatValue>(&value)) {
        if (!policy.allowDoubles) {
            fail(error, runtime::ErrorCode::DeterminismViolation,
                "floating-point value disallowed by ScriptStatePolicy");
            return false;
        }
        std::uint32_t bits = 0;
        const float norm = v->value == 0.0f ? 0.0f : v->value;
        std::memcpy(&bits, &norm, sizeof(bits));
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "float:0x%08x", bits);
        outLines[keyPrefix] = buf;
        return true;
    }
    if (const auto* v = std::get_if<double>(&value)) {
        if (!policy.allowDoubles) {
            fail(error, runtime::ErrorCode::DeterminismViolation,
                "floating-point value disallowed by ScriptStatePolicy");
            return false;
        }
        const std::uint64_t bits = canonicalDoubleBits(*v);
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "double:0x%016llx", static_cast<unsigned long long>(bits));
        outLines[keyPrefix] = buf;
        return true;
    }
    if (const auto* v = std::get_if<runtime::FixedValue>(&value)) {
        outLines[keyPrefix] = "fixed:" + std::to_string(v->raw);
        return true;
    }
    if (const auto* v = std::get_if<runtime::Fixed16Value>(&value)) {
        outLines[keyPrefix] = "fixed16:" + std::to_string(v->raw);
        return true;
    }
    if (const auto* v = std::get_if<runtime::Vec2Value>(&value)) {
        outLines[keyPrefix] = "vec2:" + std::to_string(v->x) + "," + std::to_string(v->y);
        return true;
    }
    if (const auto* v = std::get_if<runtime::Vec3Value>(&value)) {
        outLines[keyPrefix] = "vec3:" + std::to_string(v->x) + "," +
            std::to_string(v->y) + "," + std::to_string(v->z);
        return true;
    }
    if (runtime::QuatValue qv{}; value.tryGetQuat(qv)) {
        outLines[keyPrefix] = "quat:" + std::to_string(qv.x) + "," +
            std::to_string(qv.y) + "," + std::to_string(qv.z) + "," + std::to_string(qv.w);
        return true;
    }
    if (runtime::EnumValue ev{}; value.tryGetEnum(ev)) {
        std::string enumTypeName;
        if (const auto* enumDesc = findProgramTypeById(program, ev.typeId)) {
            enumTypeName = semantic::canonicalTypeName(*enumDesc);
        }
        const AssetGuid enumGuid = guidForTypeHelper(guidRegistry, enumTypeName);
        outLines[keyPrefix] = "enum:" + enumGuid.toString() + "|" +
            escapeTextString(enumTypeName) + "|" +
            std::to_string(ev.typeId) + "|" +
            std::to_string(ev.value);
        return true;
    }
    if (const auto* sv = std::get_if<std::string>(&value)) {
        if (!policy.allowStrings) {
            fail(error, runtime::ErrorCode::DeterminismViolation,
                "script state contains a string disallowed by policy");
            return false;
        }
        if (allowAssetPathBinding && guidRegistry) {
            if (const auto assetGuid = guidRegistry->findGuidForAssetPath(*sv)) {
                outLines[keyPrefix] = "asset_path:" + assetGuid->toString() + "|" +
                    escapeTextString(AssetGuid::normalizeAssetPath(*sv));
                return true;
            }
        }
        outLines[keyPrefix] = "str:" + escapeTextString(*sv);
        return true;
    }
    if (const auto structTid = runtime::structValueTypeId(value)) {
        const auto* structDesc = findProgramTypeById(program, *structTid);
        const std::string structTypeName = structDesc
            ? semantic::canonicalTypeName(*structDesc)
            : resolveBuiltinStructNameById(*structTid);
        const AssetGuid structGuid = guidForTypeHelper(guidRegistry, structTypeName);

        struct SubFieldEntry {
            std::string name;
            std::string declaredTypeName;
            runtime::Value value;
        };
        std::vector<SubFieldEntry> subFields;
        if (const auto* sItem = std::get_if<runtime::StructValue>(&value)) {
            if (!sItem->valid()) return false;
            const std::size_t fCount = sItem->fieldCount();
            subFields.reserve(fCount);
            for (std::size_t idx = 0; idx < fCount; ++idx) {
                const auto elem = runtime::structValueFieldAt(*sItem, idx);
                if (!elem) return false;
                std::string fName = (structDesc && idx < structDesc->fields.size())
                    ? structDesc->fields[idx].name
                    : ("#" + formatZeroPaddedIndex(idx));
                std::string fTypeName = (structDesc && idx < structDesc->fields.size())
                    ? structDesc->fields[idx].typeName
                    : std::string{};
                subFields.push_back(SubFieldEntry{std::move(fName), std::move(fTypeName), *elem});
            }
        } else {
            for (std::size_t idx = 0;; ++idx) {
                const auto elem = runtime::loadInlineStructField(value, idx);
                if (!elem) break;
                std::string fName = (structDesc && idx < structDesc->fields.size())
                    ? structDesc->fields[idx].name
                    : ("#" + formatZeroPaddedIndex(idx));
                std::string fTypeName = (structDesc && idx < structDesc->fields.size())
                    ? structDesc->fields[idx].typeName
                    : std::string{};
                subFields.push_back(SubFieldEntry{std::move(fName), std::move(fTypeName), *elem});
            }
        }

        outLines[keyPrefix] = "struct:" + structGuid.toString() + "|" +
            escapeTextString(structTypeName) + "|" +
            std::to_string(*structTid) + "|" +
            std::to_string(subFields.size());
        for (const auto& sf : subFields) {
            const bool sfAllowAssetPath =
                memberAllowsAssetPathBinding(graphCtx.metadata, structTypeName, sf.name);
            if (!encodeValueLines(
                    runtime,
                    scene,
                    guidRegistry,
                    keyPrefix + "." + sf.name,
                    sf.value,
                    policy,
                    depth + 1u,
                    graphCtx,
                    outLines,
                    error,
                    sfAllowAssetPath,
                    sf.declaredTypeName)) {
                return false;
            }
        }
        return true;
    }

    fail(error, runtime::ErrorCode::DeterminismViolation,
        "unsupported runtime value in text serializer");
    return false;
}

bool decodeValueLines(
    ScriptRuntime* runtime,
    SceneScriptRuntime* scene,
    const AssetGuidRegistry* guidRegistry,
    const std::map<std::string, std::string>& lines,
    const std::string& keyPrefix,
    const ScriptStatePolicy& policy,
    std::size_t depth,
    std::vector<runtime::PersistentRoot>& tempRoots,
    TextDecodeGraphContext& graphCtx,
    runtime::Value& out,
    runtime::RuntimeError& error) {
    const auto rootIt = lines.find(keyPrefix);
    if (rootIt == lines.end()) {
        return false;
    }
    const std::string_view spec = trimAscii(rootIt->second);
    if (spec == "void") { out = std::monostate{}; return true; }
    if (spec == "null_str") { out = runtime::NullString{}; return true; }
    if (spec == "null_obj") { out = runtime::NullObject{}; return true; }
    if (spec == "null_arr") { out = runtime::NullArray{}; return true; }

    const auto colonPos = spec.find(':');
    if (colonPos == std::string_view::npos) return false;
    const std::string_view tag = spec.substr(0, colonPos);
    const std::string_view payload = spec.substr(colonPos + 1u);

    if (depth > policy.maximumStructDepth) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "serialized text value exceeds maximum struct depth");
        return false;
    }

    if (tag == "obj_ref") {
        const std::string refId(trimAscii(payload));
        if (refId.empty()) return false;
        if (const auto foundIt = graphCtx.objectsById.find(refId);
            foundIt != graphCtx.objectsById.end()) {
            out = foundIt->second;
            return true;
        }
        if (const auto defIt = graphCtx.defKeyByRefId.find(refId);
            defIt != graphCtx.defKeyByRefId.end()) {
            if (!graphCtx.resolvingRefIds.insert(refId).second) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "unresolved cyclic object reference '" + refId + "'");
                return false;
            }
            struct ResolvingGuard {
                std::unordered_set<std::string>& set;
                std::string key;
                ~ResolvingGuard() { set.erase(key); }
            } guard{graphCtx.resolvingRefIds, refId};
            return decodeValueLines(
                runtime, scene, guidRegistry, lines, defIt->second,
                policy, depth + 1u, tempRoots, graphCtx, out, error);
        }
        fail(error, runtime::ErrorCode::InvalidProgram,
            "unknown object reference ID '" + refId + "'");
        return false;
    }

    if (tag == "bool") {
        bool b = false;
        if (!parseBoolToken(payload, b)) return false;
        out = b;
        return true;
    }
    if (tag == "byte") {
        std::uint64_t v = 0;
        if (!parseU64(payload, v) || v > 0xFFu) return false;
        out = runtime::ByteValue{static_cast<std::uint8_t>(v)};
        return true;
    }
    if (tag == "sbyte") {
        std::int64_t v = 0;
        if (!parseI64(payload, v) || v < -128 || v > 127) return false;
        out = runtime::SByteValue{static_cast<std::int8_t>(v)};
        return true;
    }
    if (tag == "short") {
        std::int64_t v = 0;
        if (!parseI64(payload, v) || v < -32768 || v > 32767) return false;
        out = runtime::ShortValue{static_cast<std::int16_t>(v)};
        return true;
    }
    if (tag == "ushort") {
        std::uint64_t v = 0;
        if (!parseU64(payload, v) || v > 0xFFFFu) return false;
        out = runtime::UShortValue{static_cast<std::uint16_t>(v)};
        return true;
    }
    if (tag == "int") {
        std::int64_t v = 0;
        if (!parseI64(payload, v)) return false;
        out = v;
        return true;
    }
    if (tag == "uint") {
        std::uint64_t v = 0;
        if (!parseU64(payload, v) || v > 0xFFFFFFFFULL) return false;
        out = runtime::UIntValue{static_cast<std::uint32_t>(v)};
        return true;
    }
    if (tag == "long") {
        std::int64_t v = 0;
        if (!parseI64(payload, v)) return false;
        out = runtime::LongValue{v};
        return true;
    }
    if (tag == "ulong") {
        std::uint64_t v = 0;
        if (!parseU64(payload, v)) return false;
        out = runtime::ULongValue{v};
        return true;
    }
    if (tag == "char") {
        std::uint64_t v = 0;
        if (!parseU64(payload, v) || v > 0xFFFFu) return false;
        out = runtime::CharValue{static_cast<char16_t>(v)};
        return true;
    }
    if (tag == "float") {
        if (!policy.allowDoubles) {
            fail(error, runtime::ErrorCode::DeterminismViolation,
                "floating-point value disallowed by ScriptStatePolicy");
            return false;
        }
        std::uint64_t bits64 = 0;
        if (!parseU64(payload, bits64) || bits64 > 0xFFFFFFFFULL) return false;
        const auto bits32 = static_cast<std::uint32_t>(bits64);
        float f = 0.0f;
        std::memcpy(&f, &bits32, sizeof(f));
        out = runtime::FloatValue{f};
        return true;
    }
    if (tag == "double") {
        if (!policy.allowDoubles) {
            fail(error, runtime::ErrorCode::DeterminismViolation,
                "floating-point value disallowed by ScriptStatePolicy");
            return false;
        }
        std::uint64_t bits64 = 0;
        if (!parseU64(payload, bits64)) return false;
        double d = 0.0;
        std::memcpy(&d, &bits64, sizeof(d));
        out = d;
        return true;
    }
    if (tag == "fixed") {
        std::int64_t raw = 0;
        if (!parseI64(payload, raw)) return false;
        out = runtime::FixedValue{static_cast<std::int32_t>(raw)};
        return true;
    }
    if (tag == "fixed16") {
        std::int64_t raw = 0;
        if (!parseI64(payload, raw)) return false;
        out = runtime::Fixed16Value{raw};
        return true;
    }
    if (tag == "vec2") {
        const auto c1 = payload.find(',');
        if (c1 == std::string_view::npos) return false;
        std::int64_t x = 0, y = 0;
        if (!parseI64(payload.substr(0, c1), x) ||
            !parseI64(payload.substr(c1 + 1u), y)) return false;
        out = runtime::Vec2Value{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
        return true;
    }
    if (tag == "vec3") {
        const auto c1 = payload.find(',');
        if (c1 == std::string_view::npos) return false;
        const auto c2 = payload.find(',', c1 + 1u);
        if (c2 == std::string_view::npos) return false;
        std::int64_t x = 0, y = 0, z = 0;
        if (!parseI64(payload.substr(0, c1), x) ||
            !parseI64(payload.substr(c1 + 1u, c2 - c1 - 1u), y) ||
            !parseI64(payload.substr(c2 + 1u), z)) return false;
        out = runtime::Vec3Value{
            static_cast<std::int32_t>(x),
            static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(z)};
        return true;
    }
    if (tag == "quat") {
        const auto c1 = payload.find(',');
        if (c1 == std::string_view::npos) return false;
        const auto c2 = payload.find(',', c1 + 1u);
        if (c2 == std::string_view::npos) return false;
        const auto c3 = payload.find(',', c2 + 1u);
        if (c3 == std::string_view::npos) return false;
        std::int64_t x = 0, y = 0, z = 0, w = 0;
        if (!parseI64(payload.substr(0, c1), x) ||
            !parseI64(payload.substr(c1 + 1u, c2 - c1 - 1u), y) ||
            !parseI64(payload.substr(c2 + 1u, c3 - c2 - 1u), z) ||
            !parseI64(payload.substr(c3 + 1u), w)) return false;
        out = runtime::QuatValue{
            static_cast<std::int32_t>(x),
            static_cast<std::int32_t>(y),
            static_cast<std::int32_t>(z),
            static_cast<std::int32_t>(w)};
        return true;
    }
    if (tag == "str") {
        if (!policy.allowStrings) {
            fail(error, runtime::ErrorCode::DeterminismViolation,
                "string value disallowed by ScriptStatePolicy");
            return false;
        }
        std::string s;
        if (!unescapeTextString(payload, s)) return false;
        out = std::move(s);
        return true;
    }
    if (tag == "asset_path") {
        if (!policy.allowStrings) {
            fail(error, runtime::ErrorCode::DeterminismViolation,
                "string value disallowed by ScriptStatePolicy");
            return false;
        }
        std::vector<std::string_view> parts;
        if (!splitTopLevelPipes(payload, parts) || parts.size() != 2u) return false;
        std::string fallbackPath;
        if (!unescapeTextString(parts[1], fallbackPath)) return false;
        out = resolveAssetPathWithGuid(guidRegistry, parts[0], fallbackPath);
        return true;
    }
    if (tag == "enum") {
        std::vector<std::string_view> parts;
        if (!splitTopLevelPipes(payload, parts) || parts.size() != 4u) return false;
        std::string fallbackType;
        if (!unescapeTextString(parts[1], fallbackType)) return false;
        std::uint64_t storedTypeId = 0;
        std::int64_t enumVal = 0;
        if (!parseU64(parts[2], storedTypeId) || !parseI64(parts[3], enumVal)) return false;
        const std::string resolvedType = resolveTypeWithGuid(guidRegistry, parts[0], fallbackType);
        semantic::SymbolId effectiveTid = !resolvedType.empty()
            ? semantic::stableTypeId(resolvedType)
            : storedTypeId;
        if (runtime) {
            if (const auto st = runtime->findType(resolvedType)) {
                effectiveTid = st->descriptor.id;
            }
        }
        out = runtime::EnumValue{effectiveTid, enumVal};
        return true;
    }
    if (tag == "comp_ref") {
        std::vector<std::string_view> parts;
        if (!splitTopLevelPipes(payload, parts) || parts.size() != 3u) return false;
        std::uint64_t entId = 0;
        std::string fallbackCompType;
        if (!parseU64(parts[0], entId) || !unescapeTextString(parts[2], fallbackCompType)) {
            return false;
        }
        const std::string resolvedCompType =
            resolveTypeWithGuid(guidRegistry, parts[1], fallbackCompType);
        if (!scene) {
            out = runtime::NullObject{};
            return true;
        }
        if (auto* compObj = scene->getComponent(entId, resolvedCompType)) {
            out = compObj->value();
            return true;
        }
        if (resolvedCompType != fallbackCompType) {
            if (auto* compObj = scene->getComponent(entId, fallbackCompType)) {
                out = compObj->value();
                return true;
            }
        }
        out = runtime::NullObject{};
        return true;
    }
    if (tag == "asset_obj_ref") {
        std::vector<std::string_view> parts;
        if (!splitTopLevelPipes(payload, parts) || parts.size() != 4u) return false;
        std::string fallbackPath;
        std::string fallbackType;
        if (!unescapeTextString(parts[1], fallbackPath) ||
            !unescapeTextString(parts[3], fallbackType)) {
            return false;
        }
        const std::string resolvedPath =
            resolveAssetPathWithGuid(guidRegistry, parts[0], fallbackPath);
        const std::string resolvedType =
            resolveTypeWithGuid(guidRegistry, parts[2], fallbackType);
        if (!runtime) {
            out = runtime::NullObject{};
            return true;
        }
        if (auto* foundAsset = runtime->findAsset(resolvedPath)) {
            out = foundAsset->value();
            return true;
        }
        if (resolvedPath != fallbackPath) {
            if (auto* oldAsset = runtime->findAsset(fallbackPath)) {
                out = oldAsset->value();
                return true;
            }
        }
        if (!resolvedType.empty()) {
            auto createdAsset = runtime->createAsset(resolvedPath, resolvedType, error);
            if (createdAsset) {
                out = createdAsset->value();
                return true;
            }
        }
        out = runtime::NullObject{};
        return true;
    }
    if (tag == "struct") {
        std::vector<std::string_view> parts;
        if (!splitTopLevelPipes(payload, parts) || parts.size() != 4u) return false;
        std::string fallbackType;
        if (!unescapeTextString(parts[1], fallbackType)) return false;
        std::uint64_t storedTid = 0;
        std::uint64_t count64 = 0;
        if (!parseU64(parts[2], storedTid) || !parseU64(parts[3], count64) ||
            count64 > policy.maximumFields) {
            return false;
        }
        const std::string resolvedType = resolveTypeWithGuid(guidRegistry, parts[0], fallbackType);
        semantic::SymbolId effectiveTid = !resolvedType.empty()
            ? semantic::stableTypeId(resolvedType)
            : storedTid;
        std::optional<ScriptType> foundSt;
        const semantic::TypeSymbol* structDesc = nullptr;
        if (runtime && runtime->program()) {
            foundSt = runtime->findType(resolvedType);
            if (foundSt) {
                effectiveTid = foundSt->descriptor.id;
                structDesc = &foundSt->descriptor;
            } else {
                structDesc = findProgramTypeById(runtime->program().get(), effectiveTid);
            }
        }

        std::vector<runtime::Value> fieldVals;
        if (structDesc && structDesc->kind == semantic::TypeKind::Struct) {
            fieldVals.resize(structDesc->fields.size());
            for (std::size_t idx = 0; idx < structDesc->fields.size(); ++idx) {
                const auto& fDesc = structDesc->fields[idx];
                const std::string subKey = keyPrefix + "." + fDesc.name;
                if (lines.find(subKey) != lines.end()) {
                    if (!decodeValueLines(
                            runtime, scene, guidRegistry, lines, subKey,
                            policy, depth + 1u, tempRoots, graphCtx, fieldVals[idx], error)) {
                        return false;
                    }
                } else {
                    const std::string posKey = keyPrefix + ".#" + formatZeroPaddedIndex(idx);
                    if (lines.find(posKey) != lines.end()) {
                        if (!decodeValueLines(
                                runtime, scene, guidRegistry, lines, posKey,
                                policy, depth + 1u, tempRoots, graphCtx, fieldVals[idx], error)) {
                            return false;
                        }
                    }
                }
            }
        } else {
            const auto count = static_cast<std::size_t>(count64);
            fieldVals.resize(count);
            // Collect either positional `#000000` keys or lexicographical `.field` subkeys
            std::vector<std::string> directSubKeys;
            const std::string dotPrefix = keyPrefix + ".";
            for (auto it = lines.lower_bound(dotPrefix);
                 it != lines.end() && it->first.rfind(dotPrefix, 0) == 0;
                 ++it) {
                const std::string_view rest(
                    it->first.data() + dotPrefix.size(),
                    it->first.size() - dotPrefix.size());
                if (rest.find('.') == std::string_view::npos &&
                    rest.find('[') == std::string_view::npos) {
                    directSubKeys.push_back(it->first);
                }
            }
            if (directSubKeys.size() != count) return false;
            for (std::size_t idx = 0; idx < count; ++idx) {
                if (!decodeValueLines(
                        runtime, scene, guidRegistry, lines, directSubKeys[idx],
                        policy, depth + 1u, tempRoots, graphCtx, fieldVals[idx], error)) {
                    return false;
                }
            }
        }

        if (auto inlineVal = runtime::makeDefaultInlineStructValue(effectiveTid)) {
            for (std::size_t idx = 0; idx < fieldVals.size(); ++idx) {
                auto updated = runtime::storeInlineStructField(*inlineVal, idx, fieldVals[idx]);
                if (!updated) return false;
                inlineVal = std::move(updated);
            }
            out = std::move(*inlineVal);
            return true;
        }
        out = runtime::makeStructValueFromFields(effectiveTid, std::move(fieldVals));
        return true;
    }
    if (tag == "array") {
        if (!runtime || !runtime->heap()) {
            fail(error, runtime::ErrorCode::InvalidObjectReference,
                "cannot materialize heap array without an active ScriptRuntime");
            return false;
        }
        std::vector<std::string_view> parts;
        if (!splitTopLevelPipes(payload, parts) ||
            (parts.size() != 4u && parts.size() != 5u)) {
            return false;
        }
        const std::string objRefId = parts.size() == 5u ? std::string(parts[4]) : std::string{};
        if (!objRefId.empty()) {
            if (const auto existingIt = graphCtx.objectsById.find(objRefId);
                existingIt != graphCtx.objectsById.end()) {
                out = existingIt->second;
                return true;
            }
        }
        const auto elemPrimOpt = parsePrimitiveTypeName(parts[0]);
        std::string fallbackElemType;
        std::uint64_t len64 = 0;
        if (!elemPrimOpt || !unescapeTextString(parts[2], fallbackElemType) ||
            !parseU64(parts[3], len64) || len64 > policy.maximumFields) {
            return false;
        }
        const semantic::PrimitiveType elemPrim = *elemPrimOpt;
        const std::string resolvedElemType =
            resolveTypeWithGuid(guidRegistry, parts[1], fallbackElemType);
        semantic::SymbolId elemTid =
            semantic::isExactType(elemPrim) && !resolvedElemType.empty()
                ? semantic::stableTypeId(resolvedElemType)
                : 0;
        if (elemTid != 0 && runtime) {
            if (const auto foundElemSt = runtime->findType(resolvedElemType)) {
                elemTid = foundElemSt->descriptor.id;
            }
        }
        const std::string arrTypeName = semantic::arrayTypeName(elemPrim, resolvedElemType);
        const semantic::SymbolId arrTid = semantic::stableTypeId(arrTypeName);

        const auto len = static_cast<std::size_t>(len64);
        const runtime::Value defaultElem =
            makeDefaultTypedArrayElement(runtime, elemPrim, elemTid, depth);
        const auto arrRef = runtime->heap()->allocateTypedArray(
            arrTid, elemPrim, elemTid, len, defaultElem, &error);
        if (!arrRef) return false;
        tempRoots.push_back(runtime->heap()->retain(*arrRef));
        if (!objRefId.empty()) {
            graphCtx.objectsById.emplace(objRefId, *arrRef);
        }
        for (std::size_t i = 0; i < len; ++i) {
            const std::string elemKey = keyPrefix + "[" + formatZeroPaddedIndex(i) + "]";
            runtime::Value elemVal{};
            if (!decodeValueLines(
                    runtime, scene, guidRegistry, lines, elemKey,
                    policy, depth + 1u, tempRoots, graphCtx, elemVal, error)) {
                return false;
            }
            if (!runtime->heap()->arraySet(*arrRef, i, elemVal, &error)) {
                return false;
            }
        }
        out = *arrRef;
        return true;
    }
    if (tag == "object") {
        if (!runtime || !runtime->heap()) {
            fail(error, runtime::ErrorCode::InvalidObjectReference,
                "cannot materialize heap object without an active ScriptRuntime");
            return false;
        }
        std::vector<std::string_view> parts;
        if (!splitTopLevelPipes(payload, parts) ||
            (parts.size() != 2u && parts.size() != 3u)) {
            return false;
        }
        const std::string objRefId = parts.size() == 3u ? std::string(parts[2]) : std::string{};
        if (!objRefId.empty()) {
            if (const auto existingIt = graphCtx.objectsById.find(objRefId);
                existingIt != graphCtx.objectsById.end()) {
                out = existingIt->second;
                return true;
            }
        }
        std::string fallbackType;
        if (!unescapeTextString(parts[1], fallbackType)) return false;
        const std::string resolvedType = resolveTypeWithGuid(guidRegistry, parts[0], fallbackType);
        auto created = runtime->createObject(resolvedType, error);
        if (!created) {
            if (const auto fb = runtime->findType(resolvedType)) {
                error = {};
                created = runtime->allocateUninitializedObject(*fb, error);
            }
        }
        if (!created) return false;
        tempRoots.push_back(runtime->heap()->retain(created->reference()));
        if (!objRefId.empty()) {
            graphCtx.objectsById.emplace(objRefId, created->reference());
        }
        const auto& objDesc = created->type().descriptor;
        for (std::size_t i = 0; i < objDesc.fields.size(); ++i) {
            const auto& fld = objDesc.fields[i];
            if (fld.staticField || fld.constField || fld.synthetic) continue;
            const std::string fKey = keyPrefix + "." + fld.name;
            if (lines.find(fKey) == lines.end()) continue;
            runtime::Value fVal{};
            if (!decodeValueLines(
                    runtime, scene, guidRegistry, lines, fKey,
                    policy, depth + 1u, tempRoots, graphCtx, fVal, error)) {
                return false;
            }
            if (fld.type == semantic::PrimitiveType::String) {
                if (const auto* sv = std::get_if<std::string>(&fVal)) {
                    const auto sRef = runtime->heap()->allocateString(*sv, &error);
                    if (!sRef) return false;
                    fVal = *sRef;
                }
            }
            if (!runtime->heap()->fieldSet(created->reference(), i, fVal, &error)) {
                return false;
            }
        }
        out = created->reference();
        return true;
    }

    return false;
}

bool parseKeyValueDocument(
    std::string_view text,
    std::size_t maxBytes,
    std::map<std::string, std::string>& outLines,
    runtime::RuntimeError& error) {
    if (text.empty() || text.size() > maxBytes) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "serialized text size is empty or exceeds maximumEncodedBytes");
        return false;
    }
    outLines.clear();
    std::size_t pos = 0;
    std::size_t lineNo = 0;
    while (pos < text.size()) {
        ++lineNo;
        const std::size_t nl = text.find('\n', pos);
        std::string_view line = (nl == std::string_view::npos)
            ? text.substr(pos)
            : text.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() : (nl + 1u);

        line = trimAscii(line);
        if (line.empty() || line.front() == '#') continue;

        // Split `key = value` outside of double quotes
        bool inQuotes = false;
        std::size_t eqPos = std::string_view::npos;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char ch = line[i];
            if (inQuotes) {
                if (ch == '\\') {
                    if (i + 1u >= line.size()) {
                        fail(error, runtime::ErrorCode::InvalidProgram,
                            "dangling escape on line " + std::to_string(lineNo));
                        return false;
                    }
                    ++i;
                } else if (ch == '"') {
                    inQuotes = false;
                }
            } else {
                if (ch == '"') {
                    inQuotes = true;
                } else if (ch == '=') {
                    eqPos = i;
                    break;
                }
            }
        }
        if (inQuotes || eqPos == std::string_view::npos) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "malformed key = value assignment on line " + std::to_string(lineNo));
            return false;
        }
        const std::string key(trimAscii(line.substr(0, eqPos)));
        const std::string val(trimAscii(line.substr(eqPos + 1u)));
        if (key.empty()) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "empty property key on line " + std::to_string(lineNo));
            return false;
        }
        if (!outLines.emplace(key, val).second) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "duplicate property key '" + key + "' on line " + std::to_string(lineNo));
            return false;
        }
    }
    return true;
}

std::string findExplicitProgramGuidAttr(
    const runtime::ProgramImage& program,
    const std::string& canonicalTypeName) {
    for (const auto& mod : program.modules()) {
        for (const auto& attr : mod.languageMetadata.attributes) {
            if (attr.target == canonicalTypeName &&
                (attr.name == "Guid" || attr.name == "AssetGuid") &&
                !attr.arguments.empty()) {
                std::string raw = attr.arguments.front().value;
                if (raw.size() >= 2u && raw.front() == '"' && raw.back() == '"') {
                    raw = raw.substr(1u, raw.size() - 2u);
                }
                if (!raw.empty()) return raw;
            }
        }
    }
    return {};
}

AssetGuid guidForTypeWithProgramHelper(
    const AssetGuidRegistry* guidRegistry,
    const runtime::ProgramImage* program,
    std::string_view canonicalTypeName) {
    if (canonicalTypeName.empty()) return AssetGuid::nil();
    if (guidRegistry) {
        if (const auto found = guidRegistry->findGuidForType(canonicalTypeName)) {
            return *found;
        }
    }
    if (program) {
        const std::string attr =
            findExplicitProgramGuidAttr(*program, std::string(trimAscii(canonicalTypeName)));
        if (!attr.empty()) {
            if (const auto parsed = AssetGuid::parse(attr); parsed && parsed->valid()) {
                return *parsed;
            }
        }
    }
    return AssetGuid::fromTypeName(canonicalTypeName);
}

} // namespace

AssetGuid AssetGuidRegistry::registerType(
    std::string_view canonicalTypeName,
    std::optional<AssetGuid> explicitGuid) {
    const std::string cleanType(trimAscii(canonicalTypeName));
    if (cleanType.empty()) return AssetGuid::nil();

    AssetGuid guid = (explicitGuid && explicitGuid->valid())
        ? *explicitGuid
        : AssetGuid::nil();
    if (!guid.valid()) {
        if (const auto existing = guidByType_.find(cleanType); existing != guidByType_.end()) {
            return existing->second;
        }
        guid = AssetGuid::fromTypeName(cleanType);
    }

    if (const auto prevGuidIt = guidByType_.find(cleanType);
        prevGuidIt != guidByType_.end() && prevGuidIt->second != guid) {
        typeByGuid_.erase(prevGuidIt->second);
    }
    if (const auto prevTypeIt = typeByGuid_.find(guid);
        prevTypeIt != typeByGuid_.end() && prevTypeIt->second != cleanType) {
        guidByType_.erase(prevTypeIt->second);
    }
    if (const auto prevAssetIt = assetPathByGuid_.find(guid);
        prevAssetIt != assetPathByGuid_.end()) {
        guidByAssetPath_.erase(prevAssetIt->second);
        assetPathByGuid_.erase(prevAssetIt);
    }
    typeByGuid_[guid] = cleanType;
    guidByType_[cleanType] = guid;
    return guid;
}

bool AssetGuidRegistry::rebindType(
    AssetGuid guid,
    std::string_view newCanonicalTypeName) {
    const std::string cleanType(trimAscii(newCanonicalTypeName));
    if (!guid.valid() || cleanType.empty()) return false;

    if (const auto oldIt = typeByGuid_.find(guid); oldIt != typeByGuid_.end()) {
        guidByType_.erase(oldIt->second);
    }
    if (const auto prevGuidIt = guidByType_.find(cleanType);
        prevGuidIt != guidByType_.end() && prevGuidIt->second != guid) {
        typeByGuid_.erase(prevGuidIt->second);
    }
    if (const auto prevAssetIt = assetPathByGuid_.find(guid);
        prevAssetIt != assetPathByGuid_.end()) {
        guidByAssetPath_.erase(prevAssetIt->second);
        assetPathByGuid_.erase(prevAssetIt);
    }
    typeByGuid_[guid] = cleanType;
    guidByType_[cleanType] = guid;
    return true;
}

AssetGuid AssetGuidRegistry::registerRenamedType(
    std::string_view oldCanonicalTypeName,
    std::string_view newCanonicalTypeName) {
    const std::string cleanOld(trimAscii(oldCanonicalTypeName));
    if (cleanOld.empty()) return AssetGuid::nil();
    AssetGuid guid = AssetGuid::nil();
    if (const auto found = findGuidForType(cleanOld)) {
        guid = *found;
    } else {
        guid = AssetGuid::fromTypeName(cleanOld);
    }
    if (!rebindType(guid, newCanonicalTypeName)) {
        return AssetGuid::nil();
    }
    return guid;
}

AssetGuid AssetGuidRegistry::registerAssetPath(
    std::string_view assetPath,
    std::optional<AssetGuid> explicitGuid) {
    const std::string cleanPath = AssetGuid::normalizeAssetPath(assetPath);
    if (cleanPath.empty()) return AssetGuid::nil();

    AssetGuid guid = (explicitGuid && explicitGuid->valid())
        ? *explicitGuid
        : AssetGuid::nil();
    if (!guid.valid()) {
        if (const auto existing = guidByAssetPath_.find(cleanPath);
            existing != guidByAssetPath_.end()) {
            return existing->second;
        }
        guid = AssetGuid::fromAssetPath(cleanPath);
    }

    if (const auto prevGuidIt = guidByAssetPath_.find(cleanPath);
        prevGuidIt != guidByAssetPath_.end() && prevGuidIt->second != guid) {
        assetPathByGuid_.erase(prevGuidIt->second);
    }
    if (const auto prevPathIt = assetPathByGuid_.find(guid);
        prevPathIt != assetPathByGuid_.end() && prevPathIt->second != cleanPath) {
        guidByAssetPath_.erase(prevPathIt->second);
    }
    if (const auto prevTypeIt = typeByGuid_.find(guid);
        prevTypeIt != typeByGuid_.end()) {
        guidByType_.erase(prevTypeIt->second);
        typeByGuid_.erase(prevTypeIt);
    }
    assetPathByGuid_[guid] = cleanPath;
    guidByAssetPath_[cleanPath] = guid;
    return guid;
}

bool AssetGuidRegistry::rebindAssetPath(
    AssetGuid guid,
    std::string_view newAssetPath) {
    const std::string cleanPath = AssetGuid::normalizeAssetPath(newAssetPath);
    if (!guid.valid() || cleanPath.empty()) return false;

    if (const auto oldIt = assetPathByGuid_.find(guid); oldIt != assetPathByGuid_.end()) {
        guidByAssetPath_.erase(oldIt->second);
    }
    if (const auto prevGuidIt = guidByAssetPath_.find(cleanPath);
        prevGuidIt != guidByAssetPath_.end() && prevGuidIt->second != guid) {
        assetPathByGuid_.erase(prevGuidIt->second);
    }
    if (const auto prevTypeIt = typeByGuid_.find(guid);
        prevTypeIt != typeByGuid_.end()) {
        guidByType_.erase(prevTypeIt->second);
        typeByGuid_.erase(prevTypeIt);
    }
    assetPathByGuid_[guid] = cleanPath;
    guidByAssetPath_[cleanPath] = guid;
    return true;
}

AssetGuid AssetGuidRegistry::registerRenamedAssetPath(
    std::string_view oldAssetPath,
    std::string_view newAssetPath) {
    const std::string cleanOld = AssetGuid::normalizeAssetPath(oldAssetPath);
    if (cleanOld.empty()) return AssetGuid::nil();
    AssetGuid guid = AssetGuid::nil();
    if (const auto found = findGuidForAssetPath(cleanOld)) {
        guid = *found;
    } else {
        guid = AssetGuid::fromAssetPath(cleanOld);
    }
    if (!rebindAssetPath(guid, newAssetPath)) {
        return AssetGuid::nil();
    }
    return guid;
}

void AssetGuidRegistry::populateFromProgram(const runtime::ProgramImage& program) {
    // Pass 1: Register all explicit [Guid] / [AssetGuid] attributes so explicit claims win
    // regardless of module/type declaration order.
    std::unordered_set<std::string> explicitTypes;
    for (const auto& mod : program.modules()) {
        for (const auto& t : mod.types) {
            const std::string canon = semantic::canonicalTypeName(t);
            if (canon.empty()) continue;
            const std::string attrText = findExplicitProgramGuidAttr(program, canon);
            if (!attrText.empty()) {
                if (const auto parsed = AssetGuid::parse(attrText); parsed && parsed->valid()) {
                    explicitTypes.insert(canon);
                    (void)registerType(canon, *parsed);
                }
            }
        }
    }
    // Pass 2: Register deterministic fallback GUIDs for remaining unbound types.
    for (const auto& mod : program.modules()) {
        for (const auto& t : mod.types) {
            const std::string canon = semantic::canonicalTypeName(t);
            if (canon.empty() || explicitTypes.count(canon) != 0 ||
                guidByType_.find(canon) != guidByType_.end()) {
                continue;
            }
            const AssetGuid det = AssetGuid::fromTypeName(canon);
            if (typeByGuid_.find(det) == typeByGuid_.end() &&
                assetPathByGuid_.find(det) == assetPathByGuid_.end()) {
                (void)registerType(canon, det);
            }
        }
    }
}

void AssetGuidRegistry::populateFromProgram(const GameProgram& program) {
    if (program.program()) {
        populateFromProgram(*program.program());
    }
}

std::optional<AssetGuid> AssetGuidRegistry::findGuidForType(
    std::string_view canonicalTypeName) const {
    const std::string clean(trimAscii(canonicalTypeName));
    const auto it = guidByType_.find(clean);
    if (it != guidByType_.end()) return it->second;
    return std::nullopt;
}

AssetGuid AssetGuidRegistry::guidForTypeOrDeterministic(
    std::string_view canonicalTypeName) const {
    if (const auto found = findGuidForType(canonicalTypeName)) {
        return *found;
    }
    return AssetGuid::fromTypeName(canonicalTypeName);
}

std::optional<std::string> AssetGuidRegistry::resolveTypeName(AssetGuid guid) const {
    const auto it = typeByGuid_.find(guid);
    if (it != typeByGuid_.end()) return it->second;
    return std::nullopt;
}

std::optional<AssetGuid> AssetGuidRegistry::findGuidForAssetPath(
    std::string_view assetPath) const {
    const std::string clean = AssetGuid::normalizeAssetPath(assetPath);
    const auto it = guidByAssetPath_.find(clean);
    if (it != guidByAssetPath_.end()) return it->second;
    return std::nullopt;
}

AssetGuid AssetGuidRegistry::guidForAssetPathOrDeterministic(
    std::string_view assetPath) const {
    if (const auto found = findGuidForAssetPath(assetPath)) {
        return *found;
    }
    return AssetGuid::fromAssetPath(assetPath);
}

std::optional<std::string> AssetGuidRegistry::resolveAssetPath(AssetGuid guid) const {
    const auto it = assetPathByGuid_.find(guid);
    if (it != assetPathByGuid_.end()) return it->second;
    return std::nullopt;
}

std::optional<AssetGuid> AssetGuidRegistry::findGuid(std::string_view key) const {
    if (const auto t = findGuidForType(key)) return t;
    return findGuidForAssetPath(key);
}

std::optional<std::string> AssetGuidRegistry::resolveIdentity(AssetGuid guid) const {
    if (const auto t = resolveTypeName(guid)) return t;
    return resolveAssetPath(guid);
}

bool AssetGuidRegistry::unregisterGuid(AssetGuid guid) {
    bool removed = false;
    if (const auto tIt = typeByGuid_.find(guid); tIt != typeByGuid_.end()) {
        guidByType_.erase(tIt->second);
        typeByGuid_.erase(tIt);
        removed = true;
    }
    if (const auto aIt = assetPathByGuid_.find(guid); aIt != assetPathByGuid_.end()) {
        guidByAssetPath_.erase(aIt->second);
        assetPathByGuid_.erase(aIt);
        removed = true;
    }
    return removed;
}

void AssetGuidRegistry::clear() noexcept {
    typeByGuid_.clear();
    guidByType_.clear();
    assetPathByGuid_.clear();
    guidByAssetPath_.clear();
}

std::string AssetGuidRegistry::serializeRegistryText() const {
    std::map<std::string, std::string> lines;
    for (const auto& [guid, path] : assetPathByGuid_) {
        lines["asset." + guid.toString()] = escapeTextString(path);
    }
    for (const auto& [guid, typeName] : typeByGuid_) {
        lines["type." + guid.toString()] = escapeTextString(typeName);
    }

    std::ostringstream out;
    out << "# RealScript AssetGuid Registry v1\n";
    out << "format = realscript.guid_registry.v1\n";
    for (const auto& [k, v] : lines) {
        out << k << " = " << v << "\n";
    }
    return out.str();
}

bool AssetGuidRegistry::deserializeRegistryText(
    std::string_view text,
    runtime::RuntimeError& error) {
    error = {};
    std::map<std::string, std::string> lines;
    if (!parseKeyValueDocument(text, 16u * 1024u * 1024u, lines, error)) {
        return false;
    }
    const auto fmtIt = lines.find("format");
    if (fmtIt == lines.end() || fmtIt->second != "realscript.guid_registry.v1") {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "invalid AssetGuidRegistry text header");
        return false;
    }

    AssetGuidRegistry staged;
    std::set<std::pair<std::uint64_t, std::uint64_t>> seenGuids;
    for (const auto& [k, v] : lines) {
        if (k == "format") continue;
        if (k.rfind("type.", 0) == 0) {
            const auto guid = AssetGuid::parse(std::string_view(k).substr(5u));
            std::string typeName;
            if (!guid || !guid->valid() || !unescapeTextString(v, typeName)) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "invalid type GUID entry '" + k + "'");
                return false;
            }
            if (!seenGuids.emplace(guid->high, guid->low).second) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "duplicate GUID across registry entries '" + k + "'");
                return false;
            }
            (void)staged.registerType(typeName, *guid);
        } else if (k.rfind("asset.", 0) == 0) {
            const auto guid = AssetGuid::parse(std::string_view(k).substr(6u));
            std::string path;
            if (!guid || !guid->valid() || !unescapeTextString(v, path)) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "invalid asset GUID entry '" + k + "'");
                return false;
            }
            if (!seenGuids.emplace(guid->high, guid->low).second) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "duplicate GUID across registry entries '" + k + "'");
                return false;
            }
            (void)staged.registerAssetPath(path, *guid);
        }
    }
    *this = std::move(staged);
    return true;
}

std::string serializeSceneText(
    const SceneScriptRuntime& scene,
    runtime::RuntimeError& error,
    const AssetGuidRegistry* guidRegistry,
    ScriptStatePolicy policy) {
    error = {};
    const auto& runtime = scene.runtime();
    ScriptMetadataRegistry metadata;
    if (runtime.program()) {
        (void)metadata.populateFromProgramImage(*runtime.program());
    }
    TextEncodeGraphContext sceneGraphCtx;
    sceneGraphCtx.metadata = &metadata;

    std::ostringstream out;
    out << "# RealScript Git-Friendly Scene v1\n";
    out << "format = realscript.scene.text.v1\n";
    out << "schema_version = " << kScriptObjectStateVersion << "\n";

    // 1. Entities sorted deterministically by SceneEntityId, one single property per line,
    //    separated by entity section headers so concurrent edits on distinct entities/components
    //    have clean non-overlapping Git diff hunks.
    std::vector<SceneEntityId> sortedEntities = scene.entities();
    std::sort(sortedEntities.begin(), sortedEntities.end());

    for (const SceneEntityId entityId : sortedEntities) {
        std::map<std::string, std::string> entLines;
        const std::string ePrefix = "entity." + std::to_string(entityId);

        entLines[ePrefix + ".active_self"] = scene.isEntityActiveSelf(entityId) ? "true" : "false";
        entLines[ePrefix + ".parent"] = std::to_string(scene.getParent(entityId));
        if (const auto entName = scene.getEntityName(entityId); !entName.empty()) {
            entLines[ePrefix + ".name"] = escapeTextString(entName);
        }

        const Transform3D localTf = scene.getLocalTransform(entityId);
        entLines[ePrefix + ".transform.pos.x"] = std::to_string(localTf.position.x.raw);
        entLines[ePrefix + ".transform.pos.y"] = std::to_string(localTf.position.y.raw);
        entLines[ePrefix + ".transform.pos.z"] = std::to_string(localTf.position.z.raw);
        entLines[ePrefix + ".transform.rot.w"] = std::to_string(localTf.rotation.w.raw);
        entLines[ePrefix + ".transform.rot.x"] = std::to_string(localTf.rotation.x.raw);
        entLines[ePrefix + ".transform.rot.y"] = std::to_string(localTf.rotation.y.raw);
        entLines[ePrefix + ".transform.rot.z"] = std::to_string(localTf.rotation.z.raw);
        entLines[ePrefix + ".transform.scale.x"] = std::to_string(localTf.scale.x.raw);
        entLines[ePrefix + ".transform.scale.y"] = std::to_string(localTf.scale.y.raw);
        entLines[ePrefix + ".transform.scale.z"] = std::to_string(localTf.scale.z.raw);

        auto groups = scene.getEntityGroups(entityId);
        std::sort(groups.begin(), groups.end());
        for (const auto& grp : groups) {
            entLines[ePrefix + ".group." + escapeTextString(grp)] = "true";
        }

        if (const auto col = scene.getEntityCollider2D(entityId)) {
            entLines[ePrefix + ".collider2d.present"] = "true";
            entLines[ePrefix + ".collider2d.collision_layer"] = std::to_string(col->collisionLayer);
            entLines[ePrefix + ".collider2d.collision_mask"] = std::to_string(col->collisionMask);
            entLines[ePrefix + ".collider2d.half_extents.x"] = std::to_string(col->halfExtents.x.raw);
            entLines[ePrefix + ".collider2d.half_extents.y"] = std::to_string(col->halfExtents.y.raw);
            entLines[ePrefix + ".collider2d.is_circle"] = col->isCircle ? "true" : "false";
            entLines[ePrefix + ".collider2d.is_trigger"] = col->isTrigger ? "true" : "false";
            entLines[ePrefix + ".collider2d.layer_mask"] = std::to_string(col->layerMask);
            entLines[ePrefix + ".collider2d.radius"] = std::to_string(col->radius.raw);
        }

        if (const auto col3 = scene.getEntityCollider3D(entityId)) {
            entLines[ePrefix + ".collider3d.present"] = "true";
            entLines[ePrefix + ".collider3d.collision_layer"] = std::to_string(col3->collisionLayer);
            entLines[ePrefix + ".collider3d.collision_mask"] = std::to_string(col3->collisionMask);
            entLines[ePrefix + ".collider3d.half_extents.x"] = std::to_string(col3->halfExtents.x.raw);
            entLines[ePrefix + ".collider3d.half_extents.y"] = std::to_string(col3->halfExtents.y.raw);
            entLines[ePrefix + ".collider3d.half_extents.z"] = std::to_string(col3->halfExtents.z.raw);
            entLines[ePrefix + ".collider3d.is_sphere"] = col3->isSphere ? "true" : "false";
            entLines[ePrefix + ".collider3d.is_trigger"] = col3->isTrigger ? "true" : "false";
            entLines[ePrefix + ".collider3d.layer_mask"] = std::to_string(col3->layerMask);
            entLines[ePrefix + ".collider3d.radius"] = std::to_string(col3->radius.raw);
        }

        out << "\n# [entity " << entityId << "]\n";
        for (const auto& [k, v] : entLines) {
            out << k << " = " << v << "\n";
        }

        // Components on this entity sorted by their deterministic AssetGuid (disambiguated with #<dupIdx> on GUID collision)
        struct CompBlock {
            std::string guidStr;
            std::map<std::string, std::string> lines;
        };
        std::vector<CompBlock> compBlocks;
        std::unordered_map<std::string, std::size_t> compGuidCounts;
        const auto comps = scene.getComponents(entityId);
        for (std::size_t compOrder = 0; compOrder < comps.size(); ++compOrder) {
            const auto* compObj = comps[compOrder];
            if (!compObj || !compObj->valid()) continue;
            const auto& fields = compObj->type().descriptor.fields;
            if (fields.size() > policy.maximumFields) {
                fail(error, runtime::ErrorCode::DeterminismViolation,
                    "script object has too many fields for the state policy");
                return {};
            }

            const std::string compType = compObj->type().canonicalName();
            const AssetGuid compGuid =
                guidForTypeWithProgramHelper(guidRegistry, runtime.program().get(), compType);
            std::string guidStr = compGuid.toString();
            const std::size_t dupIdx = compGuidCounts[guidStr]++;
            if (dupIdx > 0u) {
                guidStr += "#" + std::to_string(dupIdx);
            }
            const std::string cPrefix = ePrefix + ".component." + guidStr;

            CompBlock block;
            block.guidStr = guidStr;
            block.lines[cPrefix + ".enabled"] =
                scene.isComponentEnabled(entityId, compType) ? "true" : "false";
            block.lines[cPrefix + ".order"] = std::to_string(compOrder);
            block.lines[cPrefix + ".started"] =
                scene.isComponentStarted(entityId, compType) ? "true" : "false";
            block.lines[cPrefix + ".type"] = escapeTextString(compType);

            for (std::size_t idx = 0; idx < fields.size(); ++idx) {
                const auto& field = fields[idx];
                const bool isSequenceSynthetic =
                    field.synthetic && field.name.rfind("$sequence_", 0) == 0;
                if (!isSequenceSynthetic &&
                    (field.synthetic || field.staticField || field.constField ||
                     (!field.name.empty() && field.name.front() == '$'))) {
                    continue;
                }
                if (field.name == "__native" &&
                    field.type == semantic::PrimitiveType::Handle) {
                    continue;
                }
                const auto val = runtime.heap()->fieldGet(compObj->reference(), idx);
                if (!val) {
                    fail(error, runtime::ErrorCode::InvalidObjectReference,
                        "failed to read script field '" + field.name + "'");
                    return {};
                }
                const bool allowAssetPath =
                    memberAllowsAssetPathBinding(&metadata, compType, field.name);
                if (!encodeValueLines(
                        &runtime,
                        &scene,
                        guidRegistry,
                        cPrefix + ".field." + field.name,
                        *val,
                        policy,
                        0,
                        sceneGraphCtx,
                        block.lines,
                        error,
                        allowAssetPath,
                        field.typeName)) {
                    return {};
                }
            }
            compBlocks.push_back(std::move(block));
        }

        std::sort(compBlocks.begin(), compBlocks.end(), [](const CompBlock& a, const CompBlock& b) {
            return a.guidStr < b.guidStr;
        });
        for (const auto& cb : compBlocks) {
            out << "\n# [component " << entityId << " " << cb.guidStr << "]\n";
            for (const auto& [k, v] : cb.lines) {
                out << k << " = " << v << "\n";
            }
        }
    }

    // 2. Signals sorted deterministically by content hash/key
    if (!scene.signalConnections().empty()) {
        std::map<std::string, std::string> sigLines;
        for (const auto& conn : scene.signalConnections()) {
            const AssetGuid srcGuid = guidForTypeHelper(guidRegistry, conn.sourceComponent);
            const AssetGuid dstGuid = guidForTypeHelper(guidRegistry, conn.targetComponent);
            const std::string rhs =
                std::to_string(conn.sourceEntity) + "|" +
                srcGuid.toString() + "|" +
                escapeTextString(conn.sourceComponent) + "|" +
                escapeTextString(conn.signalName) + "|" +
                std::to_string(conn.targetEntity) + "|" +
                dstGuid.toString() + "|" +
                escapeTextString(conn.targetComponent) + "|" +
                escapeTextString(conn.targetMethod);
            const AssetGuid connId = AssetGuid::fromCanonicalName("signal", rhs);
            sigLines["signal." + connId.toString()] = rhs;
        }
        out << "\n# [signals]\n";
        for (const auto& [k, v] : sigLines) {
            out << k << " = " << v << "\n";
        }
    }

    // 3. Active 2D & 3D physics overlaps sorted by entity pair
    if (!scene.activeOverlaps2D().empty() || !scene.activeOverlaps3D().empty()) {
        std::map<std::string, std::string> ovLines;
        const auto& trig2d = scene.activeOverlapWasTrigger2D();
        for (const auto& pair : scene.activeOverlaps2D()) {
            const auto tIt = trig2d.find(pair);
            const bool wt = (tIt != trig2d.end()) ? tIt->second : false;
            ovLines["overlap2d." + std::to_string(pair.first) + "_" + std::to_string(pair.second) +
                    ".was_trigger"] = wt ? "true" : "false";
        }
        const auto& trig3d = scene.activeOverlapWasTrigger3D();
        for (const auto& pair : scene.activeOverlaps3D()) {
            const auto tIt = trig3d.find(pair);
            const bool wt = (tIt != trig3d.end()) ? tIt->second : false;
            ovLines["overlap3d." + std::to_string(pair.first) + "_" + std::to_string(pair.second) +
                    ".was_trigger"] = wt ? "true" : "false";
        }
        out << "\n# [overlaps]\n";
        for (const auto& [k, v] : ovLines) {
            out << k << " = " << v << "\n";
        }
    }

    // 4. Deterministic input snapshot
    if (!scene.currentInputActions().empty() ||
        !scene.previousInputActions().empty() ||
        !scene.inputAxes2D().empty()) {
        std::map<std::string, std::string> inpLines;
        for (const auto& [k, v] : scene.currentInputActions()) {
            inpLines["input.action_cur." + escapeTextString(k)] = v ? "true" : "false";
        }
        for (const auto& [k, v] : scene.previousInputActions()) {
            inpLines["input.action_prev." + escapeTextString(k)] = v ? "true" : "false";
        }
        for (const auto& [k, v] : scene.inputAxes2D()) {
            const std::string qk = escapeTextString(k);
            inpLines["input.axis2d." + qk + ".x"] = std::to_string(v.x.raw);
            inpLines["input.axis2d." + qk + ".y"] = std::to_string(v.y.raw);
        }
        out << "\n# [input]\n";
        for (const auto& [k, v] : inpLines) {
            out << k << " = " << v << "\n";
        }
    }

    // 5. Native ECS component tables sorted by (typeGuid, entityId)
    if (runtime.heap()) {
        std::map<std::string, std::string> ecsLines;
        for (const auto& tableName : runtime.heap()->nativeEcsTableNames()) {
            const auto* entIds = runtime.heap()->nativeEcsEntityIds(tableName);
            const auto* compVals = runtime.heap()->nativeEcsComponentValues(tableName);
            if (!entIds || !compVals || entIds->size() != compVals->size() || entIds->empty()) {
                continue;
            }
            const AssetGuid tGuid = guidForTypeHelper(guidRegistry, tableName);
            const std::string tPrefix = "ecs." + tGuid.toString();
            ecsLines[tPrefix + ".type"] = escapeTextString(tableName);
            for (std::size_t i = 0; i < entIds->size(); ++i) {
                TextEncodeGraphContext ecsGraphCtx;
                ecsGraphCtx.metadata = &metadata;
                if (!encodeValueLines(
                        &runtime,
                        &scene,
                        guidRegistry,
                        tPrefix + ".entity." + std::to_string((*entIds)[i]) + ".value",
                        (*compVals)[i],
                        policy,
                        0,
                        ecsGraphCtx,
                        ecsLines,
                        error,
                        false,
                        tableName)) {
                    return {};
                }
            }
        }
        if (!ecsLines.empty()) {
            out << "\n# [ecs]\n";
            for (const auto& [k, v] : ecsLines) {
                out << k << " = " << v << "\n";
            }
        }
    }

    std::string result = out.str();
    if (result.size() > policy.maximumEncodedBytes) {
        fail(error, runtime::ErrorCode::OutOfMemory,
            "serialized scene text exceeds maximumEncodedBytes");
        return {};
    }
    return result;
}

bool deserializeSceneText(
    SceneScriptRuntime& scene,
    const std::string& text,
    runtime::RuntimeError& error,
    const AssetGuidRegistry* guidRegistry,
    ScriptStatePolicy policy) {
    error = {};
    std::map<std::string, std::string> lines;
    if (!parseKeyValueDocument(text, policy.maximumEncodedBytes, lines, error)) {
        return false;
    }

    const auto fmtIt = lines.find("format");
    const auto verIt = lines.find("schema_version");
    std::int64_t verNum = 0;
    if (fmtIt == lines.end() || fmtIt->second != "realscript.scene.text.v1" ||
        verIt == lines.end() || !parseI64(verIt->second, verNum) ||
        verNum != static_cast<std::int64_t>(kScriptObjectStateVersion)) {
        fail(error, runtime::ErrorCode::TypeMismatch,
            "scene text header or schema_version is incompatible");
        return false;
    }

    struct ParsedCompSpec {
        std::string guidStr;
        std::string resolvedTypeName;
        std::string rawTypeName;
        bool enabled = true;
        bool started = false;
        std::uint64_t order = 0;
        std::size_t attachedCompIndex = 0;
        std::string fieldPrefix;
    };

    struct ParsedEntitySpec {
        SceneEntityId id = 0;
        bool activeSelf = true;
        SceneEntityId parent = 0;
        std::string name;
        Transform3D transform = Transform3D::identity();
        std::vector<std::string> groups;
        std::optional<SceneCollider2D> col2d;
        std::optional<SceneCollider3D> col3d;
        std::vector<ParsedCompSpec> components;
    };

    std::map<SceneEntityId, ParsedEntitySpec> entitySpecs;

    // Discover all entity IDs mentioned under `entity.<id>.*`
    for (auto it = lines.lower_bound("entity.");
         it != lines.end() && it->first.rfind("entity.", 0) == 0;
         ++it) {
        const std::string_view rest(it->first.data() + 7u, it->first.size() - 7u);
        const auto dotPos = rest.find('.');
        if (dotPos == std::string_view::npos) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "malformed entity property key '" + it->first + "'");
            return false;
        }
        std::uint64_t eid = 0;
        if (!parseU64(rest.substr(0, dotPos), eid) || eid == SceneScriptRuntime::BroadcastTarget) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "invalid entity ID in '" + it->first + "'");
            return false;
        }
        auto& ent = entitySpecs[eid];
        ent.id = eid;
    }

    // Parse per-entity properties and component headers
    for (auto& [eid, ent] : entitySpecs) {
        const std::string ePrefix = "entity." + std::to_string(eid);

        if (const auto it = lines.find(ePrefix + ".active_self"); it != lines.end()) {
            if (!parseBoolToken(it->second, ent.activeSelf)) return false;
        }
        if (const auto it = lines.find(ePrefix + ".parent"); it != lines.end()) {
            if (!parseU64(it->second, ent.parent)) return false;
        }
        if (const auto it = lines.find(ePrefix + ".name"); it != lines.end()) {
            if (!unescapeTextString(it->second, ent.name)) return false;
        }

        const auto readRaw32 = [&](const std::string& suffix, Fixed& target) -> bool {
            const auto it = lines.find(ePrefix + suffix);
            if (it == lines.end()) return true;
            std::int64_t r = 0;
            if (!parseI64(it->second, r)) return false;
            target = Fixed::fromRaw(static_cast<std::int32_t>(r));
            return true;
        };
        if (!readRaw32(".transform.pos.x", ent.transform.position.x) ||
            !readRaw32(".transform.pos.y", ent.transform.position.y) ||
            !readRaw32(".transform.pos.z", ent.transform.position.z) ||
            !readRaw32(".transform.rot.x", ent.transform.rotation.x) ||
            !readRaw32(".transform.rot.y", ent.transform.rotation.y) ||
            !readRaw32(".transform.rot.z", ent.transform.rotation.z) ||
            !readRaw32(".transform.rot.w", ent.transform.rotation.w) ||
            !readRaw32(".transform.scale.x", ent.transform.scale.x) ||
            !readRaw32(".transform.scale.y", ent.transform.scale.y) ||
            !readRaw32(".transform.scale.z", ent.transform.scale.z)) {
            return false;
        }

        const std::string grpPrefix = ePrefix + ".group.";
        for (auto gIt = lines.lower_bound(grpPrefix);
             gIt != lines.end() && gIt->first.rfind(grpPrefix, 0) == 0;
             ++gIt) {
            bool activeGrp = false;
            if (!parseBoolToken(gIt->second, activeGrp)) return false;
            if (!activeGrp) continue;
            std::string grpName;
            if (!unescapeTextString(
                    std::string_view(gIt->first).substr(grpPrefix.size()), grpName)) {
                return false;
            }
            ent.groups.push_back(std::move(grpName));
        }

        if (const auto c2It = lines.find(ePrefix + ".collider2d.present");
            c2It != lines.end()) {
            bool present = false;
            if (!parseBoolToken(c2It->second, present)) return false;
            if (present) {
                SceneCollider2D c;
                if (!readRaw32(".collider2d.half_extents.x", c.halfExtents.x) ||
                    !readRaw32(".collider2d.half_extents.y", c.halfExtents.y) ||
                    !readRaw32(".collider2d.radius", c.radius)) {
                    return false;
                }
                if (const auto it = lines.find(ePrefix + ".collider2d.is_circle"); it != lines.end()) {
                    if (!parseBoolToken(it->second, c.isCircle)) return false;
                }
                if (const auto it = lines.find(ePrefix + ".collider2d.is_trigger"); it != lines.end()) {
                    if (!parseBoolToken(it->second, c.isTrigger)) return false;
                }
                std::uint64_t u = 0;
                if (const auto it = lines.find(ePrefix + ".collider2d.layer_mask"); it != lines.end()) {
                    if (!parseU64(it->second, u)) return false;
                    c.layerMask = static_cast<std::uint32_t>(u);
                }
                if (const auto it = lines.find(ePrefix + ".collider2d.collision_layer"); it != lines.end()) {
                    if (!parseU64(it->second, u)) return false;
                    c.collisionLayer = static_cast<std::uint32_t>(u);
                }
                if (const auto it = lines.find(ePrefix + ".collider2d.collision_mask"); it != lines.end()) {
                    if (!parseU64(it->second, u)) return false;
                    c.collisionMask = static_cast<std::uint32_t>(u);
                }
                ent.col2d = c;
            }
        }

        if (const auto c3It = lines.find(ePrefix + ".collider3d.present");
            c3It != lines.end()) {
            bool present = false;
            if (!parseBoolToken(c3It->second, present)) return false;
            if (present) {
                SceneCollider3D c3;
                if (!readRaw32(".collider3d.half_extents.x", c3.halfExtents.x) ||
                    !readRaw32(".collider3d.half_extents.y", c3.halfExtents.y) ||
                    !readRaw32(".collider3d.half_extents.z", c3.halfExtents.z) ||
                    !readRaw32(".collider3d.radius", c3.radius)) {
                    return false;
                }
                if (const auto it = lines.find(ePrefix + ".collider3d.is_sphere"); it != lines.end()) {
                    if (!parseBoolToken(it->second, c3.isSphere)) return false;
                }
                if (const auto it = lines.find(ePrefix + ".collider3d.is_trigger"); it != lines.end()) {
                    if (!parseBoolToken(it->second, c3.isTrigger)) return false;
                }
                std::uint64_t u = 0;
                if (const auto it = lines.find(ePrefix + ".collider3d.layer_mask"); it != lines.end()) {
                    if (!parseU64(it->second, u)) return false;
                    c3.layerMask = static_cast<std::uint32_t>(u);
                }
                if (const auto it = lines.find(ePrefix + ".collider3d.collision_layer"); it != lines.end()) {
                    if (!parseU64(it->second, u)) return false;
                    c3.collisionLayer = static_cast<std::uint32_t>(u);
                }
                if (const auto it = lines.find(ePrefix + ".collider3d.collision_mask"); it != lines.end()) {
                    if (!parseU64(it->second, u)) return false;
                    c3.collisionMask = static_cast<std::uint32_t>(u);
                }
                ent.col3d = c3;
            }
        }

        const std::string compRootPrefix = ePrefix + ".component.";
        std::set<std::string> seenCompGuids;
        for (auto cIt = lines.lower_bound(compRootPrefix);
             cIt != lines.end() && cIt->first.rfind(compRootPrefix, 0) == 0;
             ++cIt) {
            const std::string_view rest(
                cIt->first.data() + compRootPrefix.size(),
                cIt->first.size() - compRootPrefix.size());
            const auto dotPos = rest.find('.');
            if (dotPos == std::string_view::npos) continue;
            seenCompGuids.emplace(rest.substr(0, dotPos));
        }

        for (const auto& cGuidStr : seenCompGuids) {
            const std::string cPrefix = compRootPrefix + cGuidStr;
            ParsedCompSpec cs;
            cs.guidStr = cGuidStr;
            cs.fieldPrefix = cPrefix + ".field.";
            if (const auto tIt = lines.find(cPrefix + ".type"); tIt != lines.end()) {
                if (!unescapeTextString(tIt->second, cs.rawTypeName)) return false;
            }
            const auto hashPos = cGuidStr.find('#');
            const std::string_view baseGuidView = (hashPos == std::string::npos)
                ? std::string_view(cGuidStr)
                : std::string_view(cGuidStr).substr(0, hashPos);
            cs.resolvedTypeName = resolveTypeWithGuid(guidRegistry, baseGuidView, cs.rawTypeName);
            // If rawTypeName still exists in the current program AND carries the exact same explicit
            // [Guid] attribute as `baseGuidView` (e.g. two distinct classes sharing an explicit GUID),
            // preserve `rawTypeName` instead of collapsing both onto whichever class registered last.
            if (!cs.rawTypeName.empty() && scene.runtime().program()) {
                if (const auto rawT = scene.runtime().findType(cs.rawTypeName)) {
                    const std::string rawAttr =
                        findExplicitProgramGuidAttr(*scene.runtime().program(), rawT->canonicalName());
                    if (!rawAttr.empty()) {
                        const auto parsedBase = AssetGuid::parse(baseGuidView);
                        const auto parsedAttr = AssetGuid::parse(rawAttr);
                        if (parsedBase && parsedAttr && *parsedBase == *parsedAttr) {
                            cs.resolvedTypeName = rawT->canonicalName();
                        }
                    }
                }
            }
            if (cs.resolvedTypeName.empty()) {
                fail(error, runtime::ErrorCode::TypeMismatch,
                    "unable to resolve component type for GUID '" + cGuidStr + "'");
                return false;
            }
            if (const auto enIt = lines.find(cPrefix + ".enabled"); enIt != lines.end()) {
                if (!parseBoolToken(enIt->second, cs.enabled)) return false;
            }
            if (const auto stIt = lines.find(cPrefix + ".started"); stIt != lines.end()) {
                if (!parseBoolToken(stIt->second, cs.started)) return false;
            }
            if (const auto ordIt = lines.find(cPrefix + ".order"); ordIt != lines.end()) {
                if (!parseU64(ordIt->second, cs.order)) return false;
            }
            ent.components.push_back(std::move(cs));
        }
        std::sort(
            ent.components.begin(),
            ent.components.end(),
            [](const ParsedCompSpec& a, const ParsedCompSpec& b) {
                if (a.order != b.order) return a.order < b.order;
                return a.guidStr < b.guidStr;
            });
    }

    // Pre-validate entity slot generations, parent cycles, component types, and signals
    // BEFORE clearing existing live scene state!
    struct PreValidatedSignal {
        SceneEntityId srcEnt = 0;
        std::string srcComp;
        std::string sigName;
        SceneEntityId dstEnt = 0;
        std::string dstComp;
        std::string dstMeth;
    };
    std::vector<PreValidatedSignal> validatedSignals;
    {
        std::unordered_map<std::uint32_t, SceneEntityId> preSlotOwners;
        std::unordered_map<SceneEntityId, std::vector<ScriptType>> preEntityComps;

        const auto checkEntitySlot = [&](SceneEntityId eid) -> bool {
            const std::uint32_t slot = entityIdSlot(eid);
            const std::uint32_t gen = entityIdGeneration(eid);
            const auto [it, inserted] = preSlotOwners.emplace(slot, eid);
            if (!inserted && it->second != eid) {
                if (gen != 0u || entityIdGeneration(it->second) != 0u) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "scene text contains colliding generational entity slot handles");
                    return false;
                }
            }
            return true;
        };

        for (auto& [eid, ent] : entitySpecs) {
            if (!checkEntitySlot(eid)) return false;
            if (ent.parent != SceneScriptRuntime::BroadcastTarget) {
                if (ent.parent == eid || !checkEntitySlot(ent.parent)) {
                    if (error.code == runtime::ErrorCode::None) {
                        fail(error, runtime::ErrorCode::InvalidProgram,
                            "invalid self-parent hierarchy in deserialized scene text");
                    }
                    return false;
                }
            }
            auto& compList = preEntityComps[eid];
            std::unordered_set<std::string> seenCanonOnEntity;
            for (auto& cs : ent.components) {
                std::string attachType = cs.resolvedTypeName;
                auto foundType = scene.runtime().findType(attachType);
                if (!foundType && !cs.rawTypeName.empty()) {
                    foundType = scene.runtime().findType(cs.rawTypeName);
                    if (foundType) attachType = cs.rawTypeName;
                }
                if (!foundType ||
                    foundType->descriptor.kind != semantic::TypeKind::Class ||
                    foundType->descriptor.delegateType) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "unknown or non-component script type '" + attachType + "'");
                    return false;
                }
                cs.resolvedTypeName = foundType->canonicalName();
                if (!seenCanonOnEntity.insert(cs.resolvedTypeName).second) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "duplicate component '" + cs.resolvedTypeName + "' on entity");
                    return false;
                }
                compList.push_back(*foundType);
            }
        }

        for (const auto& [eid, ent] : entitySpecs) {
            SceneEntityId cursor = ent.parent;
            std::size_t hops = 0;
            while (cursor != SceneScriptRuntime::BroadcastTarget &&
                   hops <= entitySpecs.size() + 1u) {
                if (cursor == eid) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "cyclic parent hierarchy in deserialized scene text");
                    return false;
                }
                const auto pIt = entitySpecs.find(cursor);
                cursor = (pIt == entitySpecs.end())
                    ? SceneScriptRuntime::BroadcastTarget
                    : pIt->second.parent;
                ++hops;
            }
        }

        const auto findPreComp = [&](SceneEntityId eid, const std::string& name) -> const ScriptType* {
            const auto eIt = preEntityComps.find(eid);
            if (eIt == preEntityComps.end() || eIt->second.empty()) return nullptr;
            if (name.empty()) return &eIt->second.front();
            for (const auto& ct : eIt->second) {
                if (ct.canonicalName() == name || ct.descriptor.name == name) return &ct;
            }
            if (const auto wanted = scene.runtime().findType(name)) {
                for (const auto& ct : eIt->second) {
                    if (ct.canonicalName() == wanted->canonicalName()) return &ct;
                }
            }
            return nullptr;
        };

        for (auto sIt = lines.lower_bound("signal.");
             sIt != lines.end() && sIt->first.rfind("signal.", 0) == 0;
             ++sIt) {
            std::vector<std::string_view> parts;
            if (!splitTopLevelPipes(sIt->second, parts) || parts.size() != 8u) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "malformed signal connection entry '" + sIt->first + "'");
                return false;
            }
            std::uint64_t srcEnt = 0;
            std::uint64_t dstEnt = 0;
            std::string rawSrcComp, sigName, rawDstComp, dstMeth;
            if (!parseU64(parts[0], srcEnt) ||
                !unescapeTextString(parts[2], rawSrcComp) ||
                !unescapeTextString(parts[3], sigName) ||
                !parseU64(parts[4], dstEnt) ||
                !unescapeTextString(parts[6], rawDstComp) ||
                !unescapeTextString(parts[7], dstMeth) ||
                sigName.empty() || dstMeth.empty()) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "malformed signal connection fields in '" + sIt->first + "'");
                return false;
            }
            std::string srcComp = resolveTypeWithGuid(guidRegistry, parts[1], rawSrcComp);
            std::string dstComp = resolveTypeWithGuid(guidRegistry, parts[5], rawDstComp);
            const auto* srcType = findPreComp(srcEnt, srcComp);
            if (!srcType && !rawSrcComp.empty()) {
                srcComp = rawSrcComp;
                srcType = findPreComp(srcEnt, srcComp);
            }
            const auto* dstType = findPreComp(dstEnt, dstComp);
            if (!dstType && !rawDstComp.empty()) {
                dstComp = rawDstComp;
                dstType = findPreComp(dstEnt, dstComp);
            }
            if (!srcType || !dstType) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "signal connection '" + sigName + "' references missing entity or component");
                return false;
            }
            const std::string backingFieldName = "$event_" + sigName + "_delegate";
            const semantic::FieldSymbol* eventField = nullptr;
            for (const auto& f : srcType->descriptor.fields) {
                if (f.name == backingFieldName && f.type == semantic::PrimitiveType::Object) {
                    eventField = &f;
                    break;
                }
            }
            if (!eventField) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "signal '" + sigName + "' not found on source component");
                return false;
            }
            const auto delType = scene.runtime().findType(eventField->typeName);
            if (!delType || !delType->descriptor.delegateType) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "invalid signal delegate type for '" + sigName + "'");
                return false;
            }
            std::size_t arity = 0;
            bool hasInvoke = false;
            for (const auto& m : delType->descriptor.methods) {
                if (m.name == "Invoke") {
                    hasInvoke = true;
                    arity = m.parameters.size() > 0 ? m.parameters.size() - 1u : 0u;
                    break;
                }
            }
            if (!hasInvoke || !scene.runtime().findMethod(*dstType, dstMeth, arity)) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "signal target method '" + dstMeth + "' not found");
                return false;
            }
            validatedSignals.push_back(PreValidatedSignal{
                srcEnt,
                srcType->canonicalName(),
                std::move(sigName),
                dstEnt,
                dstType->canonicalName(),
                std::move(dstMeth),
            });
        }
    }

    scene.clearForSnapshotImport();
    if (scene.runtime().heap()) {
        scene.runtime().heap()->clearNativeEcsTables();
    }

    // Pass 1: Create entities & attach components without invoking lifecycle callbacks
    for (auto& [eid, ent] : entitySpecs) {
        scene.registerImportedEntityHandle(eid);
        scene.setLocalTransform(eid, ent.transform);
        if (!ent.name.empty()) {
            scene.setEntityName(eid, ent.name);
        }
        for (const auto& grp : ent.groups) {
            scene.addEntityToGroup(eid, grp);
        }
        if (ent.col2d) {
            scene.setEntityCollider2D(eid, *ent.col2d);
        }
        if (ent.col3d) {
            scene.setEntityCollider3D(eid, *ent.col3d);
        }
        for (auto& cs : ent.components) {
            cs.attachedCompIndex = scene.componentCount(eid);
            if (!scene.restoreComponentWithoutLifecycle(eid, cs.resolvedTypeName, cs.enabled, cs.started)) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "failed to attach deserialized component '" + cs.resolvedTypeName + "'");
                return false;
            }
        }
    }

    // Pass 2: Restore hierarchy parents
    for (const auto& [eid, ent] : entitySpecs) {
        if (ent.parent != SceneScriptRuntime::BroadcastTarget) {
            if (!scene.setParent(eid, ent.parent)) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "invalid parent hierarchy in deserialized scene text");
                return false;
            }
        }
    }

    // Pass 3: Populate component fields (including cross-entity ObjectRefs & $sequence_* state)
    auto& runtime = scene.runtime();
    std::vector<runtime::PersistentRoot> tempRoots;
    TextDecodeGraphContext sceneDecodeCtx;
    sceneDecodeCtx.indexDocumentDefinitions(lines);
    for (const auto& [eid, ent] : entitySpecs) {
        const auto compsOnEnt = scene.getComponents(eid);
        for (const auto& cs : ent.components) {
            auto* compObj = cs.attachedCompIndex < compsOnEnt.size()
                ? compsOnEnt[cs.attachedCompIndex]
                : scene.getComponent(eid, cs.resolvedTypeName);
            if (!compObj) return false;
            for (const auto& desc : compObj->type().descriptor.fields) {
                const bool isSequenceSynthetic =
                    desc.synthetic && desc.name.rfind("$sequence_", 0) == 0;
                if (!isSequenceSynthetic &&
                    (desc.synthetic || desc.staticField || desc.constField ||
                     (!desc.name.empty() && desc.name.front() == '$'))) {
                    continue;
                }
                if (desc.name == "__native" &&
                    desc.type == semantic::PrimitiveType::Handle) {
                    continue;
                }
                const std::string fKey = cs.fieldPrefix + desc.name;
                if (lines.find(fKey) == lines.end()) continue;
                runtime::Value decodedVal{};
                if (!decodeValueLines(
                        &runtime,
                        &scene,
                        guidRegistry,
                        lines,
                        fKey,
                        policy,
                        0,
                        tempRoots,
                        sceneDecodeCtx,
                        decodedVal,
                        error)) {
                    if (error.code == runtime::ErrorCode::None) {
                        fail(error, runtime::ErrorCode::TypeMismatch,
                            "failed to decode field '" + desc.name + "'");
                    }
                    return false;
                }
                if (isSequenceSynthetic) {
                    if (!runtime.heap() ||
                        !runtime.heap()->fieldSet(
                            compObj->reference(), desc.index, std::move(decodedVal), &error)) {
                        return false;
                    }
                } else if (!runtime.setMember(*compObj, desc.name, decodedVal, error)) {
                    return false;
                }
            }
        }
    }

    // Pass 4: Restore pre-validated wired signals
    for (const auto& sig : validatedSignals) {
        if (!scene.connectSignal(
                sig.srcEnt, sig.srcComp, sig.sigName, sig.dstEnt, sig.dstComp, sig.dstMeth)) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "failed to restore signal connection '" + sig.sigName + "'");
            return false;
        }
    }

    // Pass 5: Restore entity activeSelf states without firing OnEnable/OnDisable
    for (const auto& [eid, ent] : entitySpecs) {
        scene.restoreEntityActiveWithoutLifecycle(eid, ent.activeSelf);
    }

    // Pass 6: Restore 2D & 3D physics overlaps and deterministic input snapshot
    {
        std::set<std::pair<SceneEntityId, SceneEntityId>> ov2d;
        std::map<std::pair<SceneEntityId, SceneEntityId>, bool> wt2d;
        for (auto it = lines.lower_bound("overlap2d.");
             it != lines.end() && it->first.rfind("overlap2d.", 0) == 0;
             ++it) {
            const std::string_view rest(it->first.data() + 10u, it->first.size() - 10u);
            const auto us = rest.find('_');
            const auto dot = rest.find('.');
            if (us == std::string_view::npos || dot == std::string_view::npos || us >= dot) continue;
            SceneEntityId a = 0, b = 0;
            bool wt = false;
            if (!parseU64(rest.substr(0, us), a) ||
                !parseU64(rest.substr(us + 1u, dot - us - 1u), b) ||
                !parseBoolToken(it->second, wt)) {
                return false;
            }
            if (a > b) std::swap(a, b);
            ov2d.emplace(a, b);
            wt2d[{a, b}] = wt;
        }
        if (!ov2d.empty()) {
            scene.restorePhysicsOverlapState(std::move(ov2d), std::move(wt2d));
        }

        std::set<std::pair<SceneEntityId, SceneEntityId>> ov3d;
        std::map<std::pair<SceneEntityId, SceneEntityId>, bool> wt3d;
        for (auto it = lines.lower_bound("overlap3d.");
             it != lines.end() && it->first.rfind("overlap3d.", 0) == 0;
             ++it) {
            const std::string_view rest(it->first.data() + 10u, it->first.size() - 10u);
            const auto us = rest.find('_');
            const auto dot = rest.find('.');
            if (us == std::string_view::npos || dot == std::string_view::npos || us >= dot) continue;
            SceneEntityId a = 0, b = 0;
            bool wt = false;
            if (!parseU64(rest.substr(0, us), a) ||
                !parseU64(rest.substr(us + 1u, dot - us - 1u), b) ||
                !parseBoolToken(it->second, wt)) {
                return false;
            }
            if (a > b) std::swap(a, b);
            ov3d.emplace(a, b);
            wt3d[{a, b}] = wt;
        }
        if (!ov3d.empty()) {
            scene.restorePhysicsOverlapState3D(std::move(ov3d), std::move(wt3d));
        }
    }

    {
        std::map<std::string, bool> curAct;
        std::map<std::string, bool> prevAct;
        std::map<std::string, Vec2> axes2d;

        const std::string curP = "input.action_cur.";
        for (auto it = lines.lower_bound(curP);
             it != lines.end() && it->first.rfind(curP, 0) == 0;
             ++it) {
            std::string actName;
            bool pressed = false;
            if (!unescapeTextString(std::string_view(it->first).substr(curP.size()), actName) ||
                !parseBoolToken(it->second, pressed)) {
                return false;
            }
            curAct[actName] = pressed;
        }

        const std::string prevP = "input.action_prev.";
        for (auto it = lines.lower_bound(prevP);
             it != lines.end() && it->first.rfind(prevP, 0) == 0;
             ++it) {
            std::string actName;
            bool pressed = false;
            if (!unescapeTextString(std::string_view(it->first).substr(prevP.size()), actName) ||
                !parseBoolToken(it->second, pressed)) {
                return false;
            }
            prevAct[actName] = pressed;
        }

        const std::string axP = "input.axis2d.";
        for (auto it = lines.lower_bound(axP);
             it != lines.end() && it->first.rfind(axP, 0) == 0;
             ++it) {
            const std::string_view rest = std::string_view(it->first).substr(axP.size());
            if (rest.size() < 4u) continue;
            const bool isX = rest.substr(rest.size() - 2u) == ".x";
            const bool isY = rest.substr(rest.size() - 2u) == ".y";
            if (!isX && !isY) continue;
            std::string axName;
            std::int64_t raw = 0;
            if (!unescapeTextString(rest.substr(0, rest.size() - 2u), axName) ||
                !parseI64(it->second, raw)) {
                return false;
            }
            if (isX) axes2d[axName].x = Fixed::fromRaw(static_cast<std::int32_t>(raw));
            else axes2d[axName].y = Fixed::fromRaw(static_cast<std::int32_t>(raw));
        }

        if (!curAct.empty() || !prevAct.empty() || !axes2d.empty()) {
            scene.restoreInputSnapshot(std::move(curAct), std::move(prevAct), std::move(axes2d));
        }
    }

    // Pass 7: Restore native ECS tables
    if (runtime.heap()) {
        std::set<std::string> ecsTableGuids;
        for (auto it = lines.lower_bound("ecs.");
             it != lines.end() && it->first.rfind("ecs.", 0) == 0;
             ++it) {
            const std::string_view rest = std::string_view(it->first).substr(4u);
            const auto dot = rest.find('.');
            if (dot != std::string_view::npos) {
                ecsTableGuids.emplace(rest.substr(0, dot));
            }
        }
        for (const auto& tGuidStr : ecsTableGuids) {
            const std::string tPrefix = "ecs." + tGuidStr;
            std::string rawTableName;
            if (const auto tIt = lines.find(tPrefix + ".type"); tIt != lines.end()) {
                if (!unescapeTextString(tIt->second, rawTableName)) return false;
            }
            std::string resolvedTable = resolveTypeWithGuid(guidRegistry, tGuidStr, rawTableName);
            if (!runtime.findType(resolvedTable) &&
                !rawTableName.empty() &&
                runtime.findType(rawTableName)) {
                resolvedTable = rawTableName;
            }
            const std::string entPrefix = tPrefix + ".entity.";
            std::set<std::int64_t> ecsEntIds;
            for (auto eIt = lines.lower_bound(entPrefix);
                 eIt != lines.end() && eIt->first.rfind(entPrefix, 0) == 0;
                 ++eIt) {
                const std::string_view rest = std::string_view(eIt->first).substr(entPrefix.size());
                const auto dot = rest.find('.');
                if (dot == std::string_view::npos) continue;
                std::int64_t eid = 0;
                if (parseI64(rest.substr(0, dot), eid)) {
                    ecsEntIds.insert(eid);
                }
            }
            for (const std::int64_t eid : ecsEntIds) {
                const std::string vKey = entPrefix + std::to_string(eid) + ".value";
                runtime::Value v{};
                if (!decodeValueLines(
                        &runtime, &scene, guidRegistry, lines, vKey,
                        policy, 0, tempRoots, sceneDecodeCtx, v, error)) {
                    return false;
                }
                (void)runtime.heap()->setNativeEcsComponent(resolvedTable, eid, std::move(v));
            }
        }
    }

    return true;
}

std::string serializePrefabText(
    const PrefabNodeTemplate& prefab,
    runtime::RuntimeError& error,
    const AssetGuidRegistry* guidRegistry,
    const ScriptRuntime* runtime,
    ScriptStatePolicy policy) {
    error = {};
    ScriptMetadataRegistry metadata;
    if (runtime && runtime->program()) {
        (void)metadata.populateFromProgramImage(*runtime->program());
    }
    TextEncodeGraphContext prefabGraphCtx;
    prefabGraphCtx.metadata = (runtime && runtime->program()) ? &metadata : nullptr;

    struct EmittedNodeSection {
        std::string nodePath;
        std::map<std::string, std::string> nodeLines;
        std::vector<std::pair<std::string, std::map<std::string, std::string>>> compBlocks;
    };
    std::vector<EmittedNodeSection> sections;

    const auto visitNode = [&](
        const auto& self,
        const PrefabNodeTemplate& node,
        const std::string& pathKey,
        const std::string& parentPathKey,
        std::size_t siblingIndex) -> bool {
        const std::string qPath = escapeTextString(pathKey);
        const std::string nPrefix = "node." + qPath;

        EmittedNodeSection sec;
        sec.nodePath = pathKey;
        sec.nodeLines[nPrefix + ".name"] = escapeTextString(node.name);
        sec.nodeLines[nPrefix + ".parent"] = escapeTextString(parentPathKey);
        sec.nodeLines[nPrefix + ".sibling_order"] = std::to_string(siblingIndex);
        sec.nodeLines[nPrefix + ".transform.pos.x"] = std::to_string(node.localTransform.position.x.raw);
        sec.nodeLines[nPrefix + ".transform.pos.y"] = std::to_string(node.localTransform.position.y.raw);
        sec.nodeLines[nPrefix + ".transform.pos.z"] = std::to_string(node.localTransform.position.z.raw);
        sec.nodeLines[nPrefix + ".transform.rot.w"] = std::to_string(node.localTransform.rotation.w.raw);
        sec.nodeLines[nPrefix + ".transform.rot.x"] = std::to_string(node.localTransform.rotation.x.raw);
        sec.nodeLines[nPrefix + ".transform.rot.y"] = std::to_string(node.localTransform.rotation.y.raw);
        sec.nodeLines[nPrefix + ".transform.rot.z"] = std::to_string(node.localTransform.rotation.z.raw);
        sec.nodeLines[nPrefix + ".transform.scale.x"] = std::to_string(node.localTransform.scale.x.raw);
        sec.nodeLines[nPrefix + ".transform.scale.y"] = std::to_string(node.localTransform.scale.y.raw);
        sec.nodeLines[nPrefix + ".transform.scale.z"] = std::to_string(node.localTransform.scale.z.raw);

        std::unordered_map<std::string, std::size_t> compKeyCounts;
        for (std::size_t cIdx = 0; cIdx < node.components.size(); ++cIdx) {
            const auto& comp = node.components[cIdx];
            if (comp.options.initialMembers.size() > policy.maximumFields) {
                fail(error, runtime::ErrorCode::DeterminismViolation,
                    "prefab component has too many initialMembers for policy");
                return false;
            }
            const AssetGuid compGuid = guidForTypeWithProgramHelper(
                guidRegistry, runtime ? runtime->program().get() : nullptr, comp.scriptType);
            std::string compSlotKey = compGuid.toString();
            const std::size_t dupIdx = compKeyCounts[compSlotKey]++;
            if (dupIdx > 0) {
                compSlotKey += "#" + std::to_string(dupIdx);
            }
            const std::string cPrefix = nPrefix + ".component." + compSlotKey;

            std::map<std::string, std::string> cLines;
            cLines[cPrefix + ".enabled"] = comp.options.enabled ? "true" : "false";
            cLines[cPrefix + ".order"] = std::to_string(cIdx);
            cLines[cPrefix + ".type"] = escapeTextString(comp.scriptType);
            cLines[cPrefix + ".type_guid"] = compGuid.toString();

            if (!std::holds_alternative<std::monostate>(comp.options.owner)) {
                if (!encodeValueLines(
                        runtime,
                        nullptr,
                        guidRegistry,
                        cPrefix + ".owner",
                        comp.options.owner,
                        policy,
                        0,
                        prefabGraphCtx,
                        cLines,
                        error)) {
                    return false;
                }
            }

            std::string declaredMemberTypeHint;
            const std::optional<ScriptType> compScriptType =
                runtime ? runtime->findType(comp.scriptType) : std::nullopt;
            for (const auto& [memberName, memberVal] : comp.options.initialMembers) {
                declaredMemberTypeHint.clear();
                if (compScriptType) {
                    for (const auto& fd : compScriptType->descriptor.fields) {
                        if (fd.name == memberName) {
                            declaredMemberTypeHint = fd.typeName;
                            break;
                        }
                    }
                }
                const bool allowAssetPath = memberAllowsAssetPathBinding(
                    prefabGraphCtx.metadata, comp.scriptType, memberName);
                const std::string encodedMemberSeg = encodePrefabMemberKeySegment(memberName);
                if (!encodeValueLines(
                        runtime,
                        nullptr,
                        guidRegistry,
                        cPrefix + ".member." + encodedMemberSeg,
                        memberVal,
                        policy,
                        0,
                        prefabGraphCtx,
                        cLines,
                        error,
                        allowAssetPath,
                        declaredMemberTypeHint)) {
                    return false;
                }
            }
            sec.compBlocks.emplace_back(compSlotKey, std::move(cLines));
        }

        std::sort(
            sec.compBlocks.begin(),
            sec.compBlocks.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        sections.push_back(std::move(sec));

        // Disambiguate sibling child names deterministically so concurrent child additions
        // on different branches get distinct non-colliding path keys.
        std::unordered_map<std::string, std::size_t> childNameCounts;
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            const std::string baseSeg = node.children[i].name.empty()
                ? ("$unnamed_" + formatZeroPaddedIndex(i))
                : node.children[i].name;
            const std::size_t occ = childNameCounts[baseSeg]++;
            const std::string seg = occ == 0 ? baseSeg : (baseSeg + "#" + std::to_string(occ));
            const std::string childPath = pathKey + "/" + seg;
            if (!self(self, node.children[i], childPath, pathKey, i)) {
                return false;
            }
        }
        return true;
    };

    const std::string rootKey = prefab.name.empty() ? "$root" : prefab.name;
    if (!visitNode(visitNode, prefab, rootKey, "", 0)) {
        return {};
    }

    std::sort(
        sections.begin(),
        sections.end(),
        [](const EmittedNodeSection& a, const EmittedNodeSection& b) {
            return a.nodePath < b.nodePath;
        });

    std::ostringstream out;
    out << "# RealScript Git-Friendly Prefab v1\n";
    out << "format = realscript.prefab.text.v1\n";
    out << "schema_version = " << kScriptObjectStateVersion << "\n";

    for (const auto& sec : sections) {
        out << "\n# [node " << escapeTextString(sec.nodePath) << "]\n";
        for (const auto& [k, v] : sec.nodeLines) {
            out << k << " = " << v << "\n";
        }
        for (const auto& [cSlot, cLines] : sec.compBlocks) {
            out << "\n# [prefab_component " << escapeTextString(sec.nodePath) << " " << cSlot << "]\n";
            for (const auto& [k, v] : cLines) {
                out << k << " = " << v << "\n";
            }
        }
    }

    std::string result = out.str();
    if (result.size() > policy.maximumEncodedBytes) {
        fail(error, runtime::ErrorCode::OutOfMemory,
            "serialized prefab text exceeds maximumEncodedBytes");
        return {};
    }
    return result;
}

std::optional<PrefabNodeTemplate> deserializePrefabText(
    const std::string& text,
    runtime::RuntimeError& error,
    const AssetGuidRegistry* guidRegistry,
    ScriptRuntime* runtime,
    ScriptStatePolicy policy) {
    error = {};
    std::map<std::string, std::string> lines;
    if (!parseKeyValueDocument(text, policy.maximumEncodedBytes, lines, error)) {
        return std::nullopt;
    }

    const auto fmtIt = lines.find("format");
    const auto verIt = lines.find("schema_version");
    std::int64_t verNum = 0;
    if (fmtIt == lines.end() || fmtIt->second != "realscript.prefab.text.v1" ||
        verIt == lines.end() || !parseI64(verIt->second, verNum) ||
        verNum != static_cast<std::int64_t>(kScriptObjectStateVersion)) {
        fail(error, runtime::ErrorCode::TypeMismatch,
            "prefab text header or schema_version is incompatible");
        return std::nullopt;
    }

    struct ParsedPrefabNode {
        std::string pathKey;
        std::string parentKey;
        std::uint64_t siblingOrder = 0;
        PrefabNodeTemplate tmpl;
    };

    // Extract distinct quoted node path keys from `node."<escaped>".<rest>`
    std::map<std::string, ParsedPrefabNode> nodeMap;
    for (auto it = lines.lower_bound("node.\"");
         it != lines.end() && it->first.rfind("node.\"", 0) == 0;
         ++it) {
        const std::string_view kView(it->first);
        std::size_t qEnd = std::string_view::npos;
        for (std::size_t i = 6u; i < kView.size(); ++i) {
            if (kView[i] == '\\') {
                ++i;
            } else if (kView[i] == '"') {
                qEnd = i;
                break;
            }
        }
        if (qEnd == std::string_view::npos || qEnd + 1u >= kView.size() ||
            kView[qEnd + 1u] != '.') {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "malformed prefab node key '" + it->first + "'");
            return std::nullopt;
        }
        std::string pathKey;
        if (!unescapeTextString(kView.substr(5u, qEnd - 4u), pathKey)) {
            return std::nullopt;
        }
        auto& entry = nodeMap[pathKey];
        entry.pathKey = pathKey;
    }

    if (nodeMap.empty()) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "prefab text contains no node definitions");
        return std::nullopt;
    }

    std::vector<runtime::PersistentRoot> tempRoots;
    TextDecodeGraphContext prefabDecodeCtx;
    prefabDecodeCtx.indexDocumentDefinitions(lines);
    for (auto& [pathKey, pNode] : nodeMap) {
        const std::string nPrefix = "node." + escapeTextString(pathKey);
        pNode.tmpl.localTransform = Transform3D::identity();

        if (const auto it = lines.find(nPrefix + ".name"); it != lines.end()) {
            if (!unescapeTextString(it->second, pNode.tmpl.name)) return std::nullopt;
        }
        if (const auto it = lines.find(nPrefix + ".parent"); it != lines.end()) {
            if (!unescapeTextString(it->second, pNode.parentKey)) return std::nullopt;
        }
        if (const auto it = lines.find(nPrefix + ".sibling_order"); it != lines.end()) {
            if (!parseU64(it->second, pNode.siblingOrder)) return std::nullopt;
        }

        const auto readRaw32 = [&](const std::string& suffix, Fixed& target) -> bool {
            const auto it = lines.find(nPrefix + suffix);
            if (it == lines.end()) return true;
            std::int64_t r = 0;
            if (!parseI64(it->second, r)) return false;
            target = Fixed::fromRaw(static_cast<std::int32_t>(r));
            return true;
        };
        if (!readRaw32(".transform.pos.x", pNode.tmpl.localTransform.position.x) ||
            !readRaw32(".transform.pos.y", pNode.tmpl.localTransform.position.y) ||
            !readRaw32(".transform.pos.z", pNode.tmpl.localTransform.position.z) ||
            !readRaw32(".transform.rot.x", pNode.tmpl.localTransform.rotation.x) ||
            !readRaw32(".transform.rot.y", pNode.tmpl.localTransform.rotation.y) ||
            !readRaw32(".transform.rot.z", pNode.tmpl.localTransform.rotation.z) ||
            !readRaw32(".transform.rot.w", pNode.tmpl.localTransform.rotation.w) ||
            !readRaw32(".transform.scale.x", pNode.tmpl.localTransform.scale.x) ||
            !readRaw32(".transform.scale.y", pNode.tmpl.localTransform.scale.y) ||
            !readRaw32(".transform.scale.z", pNode.tmpl.localTransform.scale.z)) {
            return std::nullopt;
        }

        const std::string compRootPrefix = nPrefix + ".component.";
        std::set<std::string> compSlotKeys;
        for (auto cIt = lines.lower_bound(compRootPrefix);
             cIt != lines.end() && cIt->first.rfind(compRootPrefix, 0) == 0;
             ++cIt) {
            const std::string_view rest =
                std::string_view(cIt->first).substr(compRootPrefix.size());
            const auto dot = rest.find('.');
            if (dot != std::string_view::npos) {
                compSlotKeys.emplace(rest.substr(0, dot));
            }
        }

        struct OrderedPrefabComp {
            std::uint64_t order = 0;
            std::string slotKey;
            PrefabComponentTemplate comp;
        };
        std::vector<OrderedPrefabComp> parsedComps;
        std::unordered_set<std::string> seenPrefabCompTypes;

        for (const auto& slotKey : compSlotKeys) {
            const std::string cPrefix = compRootPrefix + slotKey;
            OrderedPrefabComp oc;
            oc.slotKey = slotKey;

            std::string rawType;
            if (const auto tIt = lines.find(cPrefix + ".type"); tIt != lines.end()) {
                if (!unescapeTextString(tIt->second, rawType)) return std::nullopt;
            }
            std::string guidStr = slotKey;
            if (const auto hashPos = guidStr.find('#'); hashPos != std::string::npos) {
                guidStr.erase(hashPos);
            }
            if (const auto tgIt = lines.find(cPrefix + ".type_guid"); tgIt != lines.end()) {
                guidStr = tgIt->second;
            }
            oc.comp.scriptType = resolveTypeWithGuid(guidRegistry, guidStr, rawType);
            if (runtime && !runtime->findType(oc.comp.scriptType) &&
                !rawType.empty() && runtime->findType(rawType)) {
                oc.comp.scriptType = rawType;
            }
            if (!rawType.empty() && runtime && runtime->program() && !guidStr.empty()) {
                if (const auto rawT = runtime->findType(rawType)) {
                    const std::string rawAttr =
                        findExplicitProgramGuidAttr(*runtime->program(), rawT->canonicalName());
                    if (!rawAttr.empty()) {
                        const auto parsedBase = AssetGuid::parse(guidStr);
                        const auto parsedAttr = AssetGuid::parse(rawAttr);
                        if (parsedBase && parsedAttr && *parsedBase == *parsedAttr) {
                            oc.comp.scriptType = rawT->canonicalName();
                        }
                    }
                }
            }
            if (!oc.comp.scriptType.empty() &&
                !seenPrefabCompTypes.emplace(oc.comp.scriptType).second &&
                runtime && runtime->program() && !guidStr.empty()) {
                if (const auto parsedGuid = AssetGuid::parse(guidStr)) {
                    std::string fallbackMatch;
                    for (const auto& mod : runtime->program()->modules()) {
                        for (const auto& type : mod.types) {
                            if (type.synthetic || type.kind != semantic::TypeKind::Class ||
                                type.interfaceType || type.delegateType) {
                                continue;
                            }
                            const std::string cand = semantic::canonicalTypeName(type);
                            if (seenPrefabCompTypes.find(cand) != seenPrefabCompTypes.end()) {
                                continue;
                            }
                            const std::string candAttr =
                                findExplicitProgramGuidAttr(*runtime->program(), cand);
                            const auto explicitGuid = candAttr.empty()
                                ? std::optional<AssetGuid>{}
                                : AssetGuid::parse(candAttr);
                            if (explicitGuid && *explicitGuid == *parsedGuid) {
                                if (!rawType.empty() && (cand == rawType || type.name == rawType)) {
                                    fallbackMatch = cand;
                                    break;
                                }
                                if (fallbackMatch.empty()) {
                                    fallbackMatch = cand;
                                }
                            }
                        }
                    }
                    if (!fallbackMatch.empty()) {
                        oc.comp.scriptType = fallbackMatch;
                        seenPrefabCompTypes.emplace(oc.comp.scriptType);
                    }
                }
            }

            if (const auto enIt = lines.find(cPrefix + ".enabled"); enIt != lines.end()) {
                if (!parseBoolToken(enIt->second, oc.comp.options.enabled)) return std::nullopt;
            }
            if (const auto ordIt = lines.find(cPrefix + ".order"); ordIt != lines.end()) {
                if (!parseU64(ordIt->second, oc.order)) return std::nullopt;
            }
            if (lines.find(cPrefix + ".owner") != lines.end()) {
                if (!decodeValueLines(
                        runtime,
                        nullptr,
                        guidRegistry,
                        lines,
                        cPrefix + ".owner",
                        policy,
                        0,
                        tempRoots,
                        prefabDecodeCtx,
                        oc.comp.options.owner,
                        error)) {
                    return std::nullopt;
                }
            }

            const std::string memPrefix = cPrefix + ".member.";
            std::map<std::string, std::string> directMemberRawSegByDecodedName;
            for (auto mIt = lines.lower_bound(memPrefix);
                 mIt != lines.end() && mIt->first.rfind(memPrefix, 0) == 0;
                 ++mIt) {
                const std::string_view rest =
                    std::string_view(mIt->first).substr(memPrefix.size());
                if (rest.empty()) {
                    fail(error, runtime::ErrorCode::InvalidProgram,
                        "empty prefab member name in '" + mIt->first + "'");
                    return std::nullopt;
                }
                if (rest.front() == '"') {
                    std::size_t qEnd = std::string_view::npos;
                    for (std::size_t i = 1u; i < rest.size(); ++i) {
                        if (rest[i] == '\\') {
                            ++i;
                        } else if (rest[i] == '"') {
                            qEnd = i;
                            break;
                        }
                    }
                    if (qEnd == std::string_view::npos ||
                        (qEnd + 1u < rest.size() &&
                         rest[qEnd + 1u] != '.' &&
                         rest[qEnd + 1u] != '[')) {
                        fail(error, runtime::ErrorCode::InvalidProgram,
                            "malformed quoted prefab member key '" + mIt->first + "'");
                        return std::nullopt;
                    }
                    const std::string rawSeg(rest.substr(0, qEnd + 1u));
                    std::string decodedName;
                    if (!unescapeTextString(rawSeg, decodedName)) {
                        return std::nullopt;
                    }
                    directMemberRawSegByDecodedName.emplace(std::move(decodedName), rawSeg);
                } else {
                    const auto dotPos = rest.find('.');
                    const auto brkPos = rest.find('[');
                    const std::size_t cut = std::min(dotPos, brkPos);
                    const std::string rawSeg(
                        cut == std::string_view::npos ? rest : rest.substr(0, cut));
                    directMemberRawSegByDecodedName.emplace(rawSeg, rawSeg);
                }
            }

            for (const auto& [decodedName, rawSeg] : directMemberRawSegByDecodedName) {
                runtime::Value mVal{};
                if (!decodeValueLines(
                        runtime,
                        nullptr,
                        guidRegistry,
                        lines,
                        memPrefix + rawSeg,
                        policy,
                        0,
                        tempRoots,
                        prefabDecodeCtx,
                        mVal,
                        error)) {
                    return std::nullopt;
                }
                oc.comp.options.initialMembers.emplace(decodedName, std::move(mVal));
            }

            parsedComps.push_back(std::move(oc));
        }

        std::sort(
            parsedComps.begin(),
            parsedComps.end(),
            [](const OrderedPrefabComp& a, const OrderedPrefabComp& b) {
                if (a.order != b.order) return a.order < b.order;
                return a.slotKey < b.slotKey;
            });
        pNode.tmpl.components.reserve(parsedComps.size());
        for (auto& oc : parsedComps) {
            pNode.tmpl.components.push_back(std::move(oc.comp));
        }
    }

    // Reconstruct tree hierarchy from parent links
    std::string rootPathKey;
    std::map<std::string, std::vector<std::string>> childKeysByParent;
    for (const auto& [pathKey, pNode] : nodeMap) {
        if (pNode.parentKey.empty()) {
            if (!rootPathKey.empty()) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "prefab text contains multiple root nodes");
                return std::nullopt;
            }
            rootPathKey = pathKey;
        } else {
            if (nodeMap.find(pNode.parentKey) == nodeMap.end()) {
                fail(error, runtime::ErrorCode::InvalidProgram,
                    "prefab node '" + pathKey + "' references missing parent '" +
                    pNode.parentKey + "'");
                return std::nullopt;
            }
            childKeysByParent[pNode.parentKey].push_back(pathKey);
        }
    }
    if (rootPathKey.empty()) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "prefab text has no root node");
        return std::nullopt;
    }

    std::unordered_set<std::string> visitedNodes;
    const auto buildTree = [&](const auto& self, const std::string& key)
        -> std::optional<PrefabNodeTemplate> {
        if (!visitedNodes.insert(key).second) {
            fail(error, runtime::ErrorCode::InvalidProgram,
                "cycle detected in prefab hierarchy at '" + key + "'");
            return std::nullopt;
        }
        auto& pNode = nodeMap.at(key);
        PrefabNodeTemplate result = std::move(pNode.tmpl);
        if (const auto chIt = childKeysByParent.find(key); chIt != childKeysByParent.end()) {
            auto cKeys = chIt->second;
            std::sort(cKeys.begin(), cKeys.end(), [&](const std::string& a, const std::string& b) {
                const auto& na = nodeMap.at(a);
                const auto& nb = nodeMap.at(b);
                if (na.siblingOrder != nb.siblingOrder) return na.siblingOrder < nb.siblingOrder;
                return a < b;
            });
            result.children.reserve(cKeys.size());
            for (const auto& ck : cKeys) {
                auto builtChild = self(self, ck);
                if (!builtChild) return std::nullopt;
                result.children.push_back(std::move(*builtChild));
            }
        }
        return result;
    };

    auto builtRoot = buildTree(buildTree, rootPathKey);
    if (!builtRoot) return std::nullopt;
    if (visitedNodes.size() != nodeMap.size()) {
        fail(error, runtime::ErrorCode::InvalidProgram,
            "prefab text contains disconnected orphan nodes");
        return std::nullopt;
    }
    return builtRoot;
}


// ============================================================================
// P3 Stage 1: Package-level ABI Contract Embedding & Verification Helpers
// ============================================================================

namespace {

std::string formatPackageU64Hex(std::uint64_t val) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::nouppercase << std::setw(16)
        << std::setfill('0') << val;
    return oss.str();
}

bool parsePackageU64Hex(const std::string& text, std::uint64_t& out) noexcept {
    if (text.empty()) return false;
    std::size_t idx = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        idx = 2;
    }
    if (idx >= text.size()) return false;
    std::uint64_t acc = 0;
    for (; idx < text.size(); ++idx) {
        const char ch = text[idx];
        unsigned d = 0;
        if (ch >= '0' && ch <= '9') d = static_cast<unsigned>(ch - '0');
        else if (base == 16 && ch >= 'a' && ch <= 'f') d = 10u + static_cast<unsigned>(ch - 'a');
        else if (base == 16 && ch >= 'A' && ch <= 'F') d = 10u + static_cast<unsigned>(ch - 'A');
        else return false;
        const auto ubase = static_cast<std::uint64_t>(base);
        if (acc > (std::numeric_limits<std::uint64_t>::max() - d) / ubase) return false;
        acc = acc * ubase + d;
    }
    out = acc;
    return true;
}

} // namespace

std::string GamePackageContractEnvelope::toJson() const {
    std::string parseErr;
    auto manifestJson = tooling::Json::parse(abiManifest.toJson(), parseErr);
    tooling::Json root = tooling::Json::Object{};
    root["packageVersion"] = static_cast<std::int64_t>(packageVersion);
    root["programContentHash"] = formatPackageU64Hex(programContentHash);
    root["hostApiHash"] = formatPackageU64Hex(hostApiHash);
    root["moduleCount"] = static_cast<std::int64_t>(moduleCount);
    root["abiManifest"] = manifestJson.value_or(tooling::Json{});
    return root.dump();
}

std::optional<GamePackageContractEnvelope> GamePackageContractEnvelope::fromJson(
    std::string_view jsonText,
    std::string* outError) {
    std::string parseErr;
    const auto parsed = tooling::Json::parse(std::string(jsonText), parseErr);
    if (!parsed || !parsed->isObject()) {
        if (outError) {
            *outError = parseErr.empty()
                ? "package contract envelope JSON must be an object"
                : parseErr;
        }
        return std::nullopt;
    }

    const auto* pkgVer = parsed->find("packageVersion");
    const auto* progHash = parsed->find("programContentHash");
    const auto* apiHash = parsed->find("hostApiHash");
    const auto* modCnt = parsed->find("moduleCount");
    const auto* manifestObj = parsed->find("abiManifest");
    if (!pkgVer || !pkgVer->isNumber() || pkgVer->integerValue(-1) < 0 ||
        !progHash || !progHash->isString() ||
        !apiHash || !apiHash->isString() ||
        !modCnt || !modCnt->isNumber() || modCnt->integerValue(-1) < 0 ||
        !manifestObj || !manifestObj->isObject()) {
        if (outError) *outError = "missing or invalid fields in GamePackageContractEnvelope JSON";
        return std::nullopt;
    }

    GamePackageContractEnvelope env{};
    env.packageVersion = static_cast<std::uint32_t>(pkgVer->integerValue());
    env.moduleCount = static_cast<std::uint32_t>(modCnt->integerValue());
    if (!parsePackageU64Hex(progHash->stringValue(), env.programContentHash) ||
        !parsePackageU64Hex(apiHash->stringValue(), env.hostApiHash)) {
        if (outError) *outError = "invalid hex hash in GamePackageContractEnvelope JSON";
        return std::nullopt;
    }

    std::string manifestErr;
    auto decodedManifest =
        AbiContractManifest::fromJson(manifestObj->dump(), &manifestErr);
    if (!decodedManifest) {
        if (outError) *outError = manifestErr;
        return std::nullopt;
    }
    env.abiManifest = std::move(*decodedManifest);
    return env;
}

AbiContractManifest buildPackageAbiManifest(
    const std::vector<bytecode::Module>& modules) noexcept {
    auto manifest = currentAbiContractManifest();
    if (!modules.empty()) {
        BytecodeVersionPoint minVer{
            modules.front().version.major, modules.front().version.minor};
        BytecodeVersionPoint maxVer = minVer;
        for (std::size_t i = 1; i < modules.size(); ++i) {
            const BytecodeVersionPoint cur{
                modules[i].version.major, modules[i].version.minor};
            if (cur < minVer) minVer = cur;
            if (cur > maxVer) maxVer = cur;
        }
        manifest.bytecodeTargetVersion =
            (minVer == BytecodeVersionPoint{0, 6} && maxVer <= manifest.bytecodeMaxVersion)
                ? minVer
                : maxVer;
        manifest.refreshLayoutDigest();
    }
    return manifest;
}

GamePackageContractEnvelope buildPackageContractEnvelope(
    const GameProgramPackage& package) noexcept {
    GamePackageContractEnvelope env{};
    env.packageVersion = kGameSdkPackageVersion;
    env.programContentHash = package.programContentHash;
    env.hostApiHash = package.hostApiHash;
    env.moduleCount = package.program
        ? static_cast<std::uint32_t>(package.program->moduleCount())
        : 0u;
    env.abiManifest = package.abiManifest;
    return env;
}

AbiCompatibilityReport verifyPackageAbiCompatibility(
    const GameProgramPackage& package,
    const AbiContractManifest& hostManifest) {
    auto report = verifyAbiCompatibility(hostManifest, package.abiManifest);
    if (package.program != nullptr) {
        for (const auto& mod : package.program->modules()) {
            const BytecodeVersionPoint modVer{mod.version.major, mod.version.minor};
            if (modVer < hostManifest.bytecodeMinVersion ||
                modVer > hostManifest.bytecodeMaxVersion) {
                report.status = AbiCompatibilityStatus::Incompatible;
                report.issues.push_back(AbiIssue{
                    AbiIssueCode::BytecodeTargetVersionUnsupported,
                    AbiCompatibilityStatus::Incompatible,
                    "RS9019",
                    "package.modules.version",
                    "module '" + mod.name + "' uses unsupported bytecode version " +
                        modVer.toString(),
                });
            } else if (modVer == BytecodeVersionPoint{0, 6} &&
                       hostManifest.bytecodeTargetVersion >= BytecodeVersionPoint{0, 7} &&
                       !report.hasIssue(AbiIssueCode::BytecodeLegacyMetadataMigration)) {
                if (report.status == AbiCompatibilityStatus::Compatible) {
                    report.status = AbiCompatibilityStatus::CompatibleWithMigration;
                }
                report.issues.push_back(AbiIssue{
                    AbiIssueCode::BytecodeLegacyMetadataMigration,
                    AbiCompatibilityStatus::CompatibleWithMigration,
                    "RS9020",
                    "package.modules.version",
                    "module '" + mod.name +
                        "' uses legacy .rsbc 0.6 metadata requiring rehydration",
                });
            }
        }
    }
    return report;
}

AbiCompatibilityReport verifyPackageContractEnvelope(
    const GamePackageContractEnvelope& envelope,
    const GameApi* expectedApi,
    std::uint64_t expectedProgramContentHash,
    const AbiContractManifest& hostManifest) {
    auto report = verifyAbiCompatibility(hostManifest, envelope.abiManifest);

    if (envelope.packageVersion == 0 ||
        envelope.packageVersion > hostManifest.gameSdkPackageVersion) {
        report.status = AbiCompatibilityStatus::Incompatible;
        report.issues.push_back(AbiIssue{
            AbiIssueCode::GameSdkPackageNewerThanHost,
            AbiCompatibilityStatus::Incompatible,
            "RS9011",
            "envelope.packageVersion",
            "envelope packageVersion (" + std::to_string(envelope.packageVersion) +
                ") is unsupported by host (" +
                std::to_string(hostManifest.gameSdkPackageVersion) + ")",
        });
    } else if (envelope.packageVersion < hostManifest.gameSdkPackageVersion &&
               !report.hasIssue(AbiIssueCode::GameSdkPackageOlderMigration)) {
        if (report.status == AbiCompatibilityStatus::Compatible) {
            report.status = AbiCompatibilityStatus::CompatibleWithMigration;
        }
        report.issues.push_back(AbiIssue{
            AbiIssueCode::GameSdkPackageOlderMigration,
            AbiCompatibilityStatus::CompatibleWithMigration,
            "RS9012",
            "envelope.packageVersion",
            "legacy envelope packageVersion requires migration",
        });
    }

    if (expectedApi != nullptr) {
        const auto currentApiHash = stableGameApiHash(*expectedApi);
        if (envelope.hostApiHash != currentApiHash) {
            report.status = AbiCompatibilityStatus::Incompatible;
            report.issues.push_back(AbiIssue{
                AbiIssueCode::SdkCompatibilityMismatch,
                AbiCompatibilityStatus::Incompatible,
                "RS7105",
                "envelope.hostApiHash",
                "package hostApiHash does not match bound GameApi contract",
            });
        }
    }

    if (expectedProgramContentHash != 0 &&
        envelope.programContentHash != expectedProgramContentHash) {
        report.status = AbiCompatibilityStatus::Incompatible;
        report.issues.push_back(AbiIssue{
            AbiIssueCode::LayoutDigestMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS7106",
            "envelope.programContentHash",
            "package programContentHash does not match expected bytecode payload",
        });
    }

    return report;
}

AbiCompatibilityReport preflightBytecodePackageHeaders(
    const std::vector<std::vector<std::uint8_t>>& encodedModules,
    const AbiContractManifest& hostManifest) {
    AbiCompatibilityReport report{};
    if (encodedModules.empty()) {
        report.status = AbiCompatibilityStatus::Incompatible;
        report.issues.push_back(AbiIssue{
            AbiIssueCode::BytecodeHeaderTruncated,
            AbiCompatibilityStatus::Incompatible,
            "RS7101",
            "encodedModules",
            "no bytecode modules were supplied for header preflight",
        });
        return report;
    }

    for (std::size_t idx = 0; idx < encodedModules.size(); ++idx) {
        const auto header =
            inspectBytecodeHeaderCompatibility(encodedModules[idx], hostManifest);
        if (static_cast<std::uint8_t>(header.status) >
            static_cast<std::uint8_t>(report.status)) {
            report.status = header.status;
        }
        for (auto issue : header.issues) {
            issue.field = "module[" + std::to_string(idx) + "]." + issue.field;
            report.issues.push_back(std::move(issue));
        }
    }
    return report;
}

} // namespace realscript::game
