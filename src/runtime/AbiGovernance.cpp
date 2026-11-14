#include "realscript/AbiGovernance.h"

#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/aot_cpp/RuntimeAbi.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/runtime/FiberScheduler.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace realscript {
namespace {

constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

constexpr void fnvMixByte(std::uint64_t& hash, std::uint8_t byte) noexcept {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kFnv1a64Prime;
}

constexpr void fnvMixU16LE(std::uint64_t& hash, std::uint16_t value) noexcept {
    fnvMixByte(hash, static_cast<std::uint8_t>(value & 0xFFu));
    fnvMixByte(hash, static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

constexpr void fnvMixU32LE(std::uint64_t& hash, std::uint32_t value) noexcept {
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        fnvMixByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

constexpr void fnvMixTag(std::uint64_t& hash, std::string_view tag) noexcept {
    fnvMixU32LE(hash, static_cast<std::uint32_t>(tag.size()));
    for (const char ch : tag) {
        fnvMixByte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(ch)));
    }
}

void elevateStatus(
    AbiCompatibilityStatus& current,
    AbiCompatibilityStatus incoming) noexcept {
    if (static_cast<std::uint8_t>(incoming) > static_cast<std::uint8_t>(current)) {
        current = incoming;
    }
}

void pushIssue(
    AbiCompatibilityReport& report,
    AbiIssueCode code,
    AbiCompatibilityStatus impact,
    const char* diagnosticCode,
    const char* field,
    std::string message) {
    elevateStatus(report.status, impact);
    report.issues.push_back(AbiIssue{
        code,
        impact,
        diagnosticCode ? std::string(diagnosticCode) : std::string{},
        field ? std::string(field) : std::string{},
        std::move(message),
    });
}

std::string formatHexU64(std::uint64_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::nouppercase << std::setw(16)
        << std::setfill('0') << value;
    return out.str();
}

bool parseHexOrDecU64(std::string_view text, std::uint64_t& out) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    if (text.empty()) return false;

    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) return false;

    std::uint64_t acc = 0;
    for (const char ch : text) {
        unsigned digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<unsigned>(ch - '0');
        } else if (base == 16 && ch >= 'a' && ch <= 'f') {
            digit = 10u + static_cast<unsigned>(ch - 'a');
        } else if (base == 16 && ch >= 'A' && ch <= 'F') {
            digit = 10u + static_cast<unsigned>(ch - 'A');
        } else {
            return false;
        }
        const auto ubase = static_cast<std::uint64_t>(base);
        if (acc > (std::numeric_limits<std::uint64_t>::max() - digit) / ubase) {
            return false;
        }
        acc = acc * ubase + digit;
    }
    out = acc;
    return true;
}

std::string escapeJsonString(std::string_view input) {
    std::ostringstream out;
    for (const char raw : input) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (raw) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20u) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(ch) << std::dec;
            } else {
                out << raw;
            }
            break;
        }
    }
    return out.str();
}

// Self-contained, zero-external-dependency strict JSON reader for AbiContractManifest
struct MiniJsonValue {
    enum class Kind { Null, Bool, Number, String, Object };
    Kind kind = Kind::Null;
    bool boolVal = false;
    std::uint64_t uintVal = 0;
    bool hasExactUint = false;
    std::string strVal;
    std::map<std::string, MiniJsonValue> objVal;

    [[nodiscard]] const MiniJsonValue* find(std::string_view key) const noexcept {
        if (kind != Kind::Object) return nullptr;
        const auto it = objVal.find(std::string(key));
        return it == objVal.end() ? nullptr : &it->second;
    }
};

class MiniJsonParser {
public:
    explicit MiniJsonParser(std::string_view src) noexcept : src_(src) {}

    bool parseRoot(MiniJsonValue& out, std::string& err) {
        skipWs();
        if (!parseValue(out, err, 0)) return false;
        skipWs();
        if (pos_ != src_.size()) {
            err = "unexpected trailing characters in ABI manifest JSON";
            return false;
        }
        return true;
    }

private:
    void skipWs() noexcept {
        while (pos_ < src_.size() &&
               std::isspace(static_cast<unsigned char>(src_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool consume(char expected) noexcept {
        skipWs();
        if (pos_ < src_.size() && src_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool parseStringRaw(std::string& out, std::string& err) {
        skipWs();
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            err = "expected '\"' starting JSON string";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char ch = src_[pos_++];
            if (ch == '"') return true;
            if (static_cast<unsigned char>(ch) < 0x20u) {
                err = "unescaped control character in JSON string";
                return false;
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= src_.size()) {
                err = "unterminated escape sequence in JSON string";
                return false;
            }
            const char esc = src_[pos_++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (pos_ + 4 > src_.size()) {
                    err = "truncated \\uXXXX escape in JSON string";
                    return false;
                }
                std::uint64_t code = 0;
                if (!parseHexOrDecU64(
                        std::string("0x") + std::string(src_.substr(pos_, 4)),
                        code)) {
                    err = "invalid hex digits in \\uXXXX escape";
                    return false;
                }
                pos_ += 4;
                if (code <= 0x7Fu) {
                    out.push_back(static_cast<char>(code));
                } else if (code <= 0x7FFu) {
                    out.push_back(static_cast<char>(0xC0u | ((code >> 6u) & 0x1Fu)));
                    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
                } else {
                    out.push_back(static_cast<char>(0xE0u | ((code >> 12u) & 0x0Fu)));
                    out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
                    out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
                }
                break;
            }
            default:
                err = "unsupported escape character in JSON string";
                return false;
            }
        }
        err = "unterminated JSON string";
        return false;
    }

    bool parseValue(MiniJsonValue& out, std::string& err, int depth) {
        if (depth > 16) {
            err = "JSON nesting depth exceeded";
            return false;
        }
        skipWs();
        if (pos_ >= src_.size()) {
            err = "unexpected end of JSON input";
            return false;
        }
        const char ch = src_[pos_];
        if (ch == '{') {
            ++pos_;
            out = MiniJsonValue{};
            out.kind = MiniJsonValue::Kind::Object;
            if (consume('}')) return true;
            while (true) {
                std::string key;
                if (!parseStringRaw(key, err)) return false;
                if (!consume(':')) {
                    err = "expected ':' after JSON object key '" + key + "'";
                    return false;
                }
                MiniJsonValue fieldVal;
                if (!parseValue(fieldVal, err, depth + 1)) return false;
                out.objVal[std::move(key)] = std::move(fieldVal);
                if (consume('}')) return true;
                if (!consume(',')) {
                    err = "expected ',' or '}' in JSON object";
                    return false;
                }
            }
        }
        if (ch == '"') {
            out = MiniJsonValue{};
            out.kind = MiniJsonValue::Kind::String;
            return parseStringRaw(out.strVal, err);
        }
        if (src_.substr(pos_, 4) == "true") {
            pos_ += 4;
            out = MiniJsonValue{};
            out.kind = MiniJsonValue::Kind::Bool;
            out.boolVal = true;
            return true;
        }
        if (src_.substr(pos_, 5) == "false") {
            pos_ += 5;
            out = MiniJsonValue{};
            out.kind = MiniJsonValue::Kind::Bool;
            out.boolVal = false;
            return true;
        }
        if (src_.substr(pos_, 4) == "null") {
            pos_ += 4;
            out = MiniJsonValue{};
            out.kind = MiniJsonValue::Kind::Null;
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            const std::size_t start = pos_;
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
                ++pos_;
            }
            if (pos_ < src_.size() &&
                (src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E')) {
                err = "floating-point numbers are not valid in integer ABI fields";
                return false;
            }
            std::uint64_t parsed = 0;
            if (!parseHexOrDecU64(src_.substr(start, pos_ - start), parsed)) {
                err = "integer literal overflow in ABI manifest JSON";
                return false;
            }
            out = MiniJsonValue{};
            out.kind = MiniJsonValue::Kind::Number;
            out.uintVal = parsed;
            out.hasExactUint = true;
            return true;
        }
        err = "unexpected token in ABI manifest JSON";
        return false;
    }

    std::string_view src_;
    std::size_t pos_ = 0;
};

bool readU32Field(
    const MiniJsonValue& obj,
    std::string_view key,
    std::uint32_t& out,
    std::string& err) {
    const auto* node = obj.find(key);
    if (!node || node->kind != MiniJsonValue::Kind::Number || !node->hasExactUint ||
        node->uintVal > std::numeric_limits<std::uint32_t>::max()) {
        err = "missing or invalid uint32 field '" + std::string(key) + "'";
        return false;
    }
    out = static_cast<std::uint32_t>(node->uintVal);
    return true;
}

bool readU16Field(
    const MiniJsonValue& obj,
    std::string_view key,
    std::uint16_t& out,
    std::string& err) {
    const auto* node = obj.find(key);
    if (!node || node->kind != MiniJsonValue::Kind::Number || !node->hasExactUint ||
        node->uintVal > std::numeric_limits<std::uint16_t>::max()) {
        err = "missing or invalid uint16 field '" + std::string(key) + "'";
        return false;
    }
    out = static_cast<std::uint16_t>(node->uintVal);
    return true;
}

bool readVersionPointField(
    const MiniJsonValue& parent,
    std::string_view key,
    BytecodeVersionPoint& out,
    std::string& err) {
    const auto* node = parent.find(key);
    if (!node || node->kind != MiniJsonValue::Kind::Object) {
        err = "missing or invalid bytecode version object '" + std::string(key) + "'";
        return false;
    }
    return readU16Field(*node, "major", out.major, err) &&
        readU16Field(*node, "minor", out.minor, err);
}

std::uint16_t readLeU16(const std::uint8_t* ptr) noexcept {
    return static_cast<std::uint16_t>(ptr[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(ptr[1]) << 8u);
}

std::uint32_t readLeU32(const std::uint8_t* ptr) noexcept {
    return static_cast<std::uint32_t>(ptr[0]) |
        (static_cast<std::uint32_t>(ptr[1]) << 8u) |
        (static_cast<std::uint32_t>(ptr[2]) << 16u) |
        (static_cast<std::uint32_t>(ptr[3]) << 24u);
}

} // namespace

std::string BytecodeVersionPoint::toString() const {
    return std::to_string(major) + "." + std::to_string(minor);
}

std::uint64_t AbiContractManifest::computeLayoutDigest() const noexcept {
    std::uint64_t hash = kFnv1a64OffsetBasis;
    fnvMixTag(hash, "RealScript.AbiContractManifest.v1");
    fnvMixU32LE(hash, schemaVersion);
    fnvMixU32LE(hash, engineVersionMajor);
    fnvMixU32LE(hash, engineVersionMinor);
    fnvMixU32LE(hash, engineVersionPatch);
    fnvMixU32LE(hash, sdkCompatibilityVersion);
    fnvMixU32LE(hash, gameSdkPackageVersion);
    fnvMixU32LE(hash, scriptObjectStateVersion);
    fnvMixU16LE(hash, bytecodeMinVersion.major);
    fnvMixU16LE(hash, bytecodeMinVersion.minor);
    fnvMixU16LE(hash, bytecodeMaxVersion.major);
    fnvMixU16LE(hash, bytecodeMaxVersion.minor);
    fnvMixU16LE(hash, bytecodeTargetVersion.major);
    fnvMixU16LE(hash, bytecodeTargetVersion.minor);
    fnvMixU32LE(hash, rsgsStateVersion);
    fnvMixU32LE(hash, runtimeValueSize);
    fnvMixU32LE(hash, runtimeValueAlign);
    fnvMixU32LE(hash, bytecodeInstructionSize);
    fnvMixU32LE(hash, bytecodeInstructionAlign);
    fnvMixU32LE(hash, compactInstructionSize);
    fnvMixU32LE(hash, nativeModuleAbi.abiMajor);
    fnvMixU32LE(hash, nativeModuleAbi.abiMinor);
    fnvMixU32LE(hash, nativeModuleAbi.runtimeApiStructSize);
    fnvMixU32LE(hash, nativeModuleAbi.functionEntryStructSize);
    fnvMixU32LE(hash, nativeModuleAbi.moduleExportsStructSize);
    if (hash == 0) hash = 1;
    return hash;
}

std::string AbiContractManifest::toJson() const {
    std::ostringstream out;
    out << "{\n"
        << "  \"schemaVersion\": " << schemaVersion << ",\n"
        << "  \"engineVersion\": {\n"
        << "    \"major\": " << engineVersionMajor << ",\n"
        << "    \"minor\": " << engineVersionMinor << ",\n"
        << "    \"patch\": " << engineVersionPatch << "\n"
        << "  },\n"
        << "  \"sdkCompatibilityVersion\": " << sdkCompatibilityVersion << ",\n"
        << "  \"gameSdkPackageVersion\": " << gameSdkPackageVersion << ",\n"
        << "  \"scriptObjectStateVersion\": " << scriptObjectStateVersion << ",\n"
        << "  \"bytecodeMinVersion\": {\"major\": " << bytecodeMinVersion.major
        << ", \"minor\": " << bytecodeMinVersion.minor << "},\n"
        << "  \"bytecodeMaxVersion\": {\"major\": " << bytecodeMaxVersion.major
        << ", \"minor\": " << bytecodeMaxVersion.minor << "},\n"
        << "  \"bytecodeTargetVersion\": {\"major\": " << bytecodeTargetVersion.major
        << ", \"minor\": " << bytecodeTargetVersion.minor << "},\n"
        << "  \"rsgsStateVersion\": " << rsgsStateVersion << ",\n"
        << "  \"runtimeValueSize\": " << runtimeValueSize << ",\n"
        << "  \"runtimeValueAlign\": " << runtimeValueAlign << ",\n"
        << "  \"bytecodeInstructionSize\": " << bytecodeInstructionSize << ",\n"
        << "  \"bytecodeInstructionAlign\": " << bytecodeInstructionAlign << ",\n"
        << "  \"compactInstructionSize\": " << compactInstructionSize << ",\n"
        << "  \"nativeModuleAbi\": {\n"
        << "    \"abiMajor\": " << nativeModuleAbi.abiMajor << ",\n"
        << "    \"abiMinor\": " << nativeModuleAbi.abiMinor << ",\n"
        << "    \"runtimeApiStructSize\": " << nativeModuleAbi.runtimeApiStructSize << ",\n"
        << "    \"functionEntryStructSize\": " << nativeModuleAbi.functionEntryStructSize << ",\n"
        << "    \"moduleExportsStructSize\": " << nativeModuleAbi.moduleExportsStructSize << "\n"
        << "  },\n"
        << "  \"layoutDigest\": \"" << escapeJsonString(formatHexU64(layoutDigest)) << "\"\n"
        << "}";
    return out.str();
}

std::optional<AbiContractManifest> AbiContractManifest::fromJson(
    std::string_view jsonText,
    std::string* outError) {
    std::string localErr;
    MiniJsonValue root;
    MiniJsonParser parser(jsonText);
    if (!parser.parseRoot(root, localErr)) {
        if (outError) *outError = localErr;
        return std::nullopt;
    }
    if (root.kind != MiniJsonValue::Kind::Object) {
        if (outError) *outError = "ABI manifest JSON root must be an object";
        return std::nullopt;
    }

    AbiContractManifest manifest{};
    if (!readU32Field(root, "schemaVersion", manifest.schemaVersion, localErr)) {
        if (outError) *outError = localErr;
        return std::nullopt;
    }
    const auto* engineNode = root.find("engineVersion");
    if (!engineNode || engineNode->kind != MiniJsonValue::Kind::Object) {
        if (outError) *outError = "missing or invalid 'engineVersion' object";
        return std::nullopt;
    }
    if (!readU32Field(*engineNode, "major", manifest.engineVersionMajor, localErr) ||
        !readU32Field(*engineNode, "minor", manifest.engineVersionMinor, localErr) ||
        !readU32Field(*engineNode, "patch", manifest.engineVersionPatch, localErr) ||
        !readU32Field(root, "sdkCompatibilityVersion", manifest.sdkCompatibilityVersion, localErr) ||
        !readU32Field(root, "gameSdkPackageVersion", manifest.gameSdkPackageVersion, localErr) ||
        !readU32Field(root, "scriptObjectStateVersion", manifest.scriptObjectStateVersion, localErr) ||
        !readVersionPointField(root, "bytecodeMinVersion", manifest.bytecodeMinVersion, localErr) ||
        !readVersionPointField(root, "bytecodeMaxVersion", manifest.bytecodeMaxVersion, localErr) ||
        !readVersionPointField(root, "bytecodeTargetVersion", manifest.bytecodeTargetVersion, localErr) ||
        !readU32Field(root, "rsgsStateVersion", manifest.rsgsStateVersion, localErr) ||
        !readU32Field(root, "runtimeValueSize", manifest.runtimeValueSize, localErr) ||
        !readU32Field(root, "runtimeValueAlign", manifest.runtimeValueAlign, localErr) ||
        !readU32Field(root, "bytecodeInstructionSize", manifest.bytecodeInstructionSize, localErr) ||
        !readU32Field(root, "bytecodeInstructionAlign", manifest.bytecodeInstructionAlign, localErr)) {
        if (outError) *outError = localErr;
        return std::nullopt;
    }

    if (root.find("compactInstructionSize") != nullptr) {
        if (!readU32Field(
                root, "compactInstructionSize", manifest.compactInstructionSize, localErr)) {
            if (outError) *outError = localErr;
            return std::nullopt;
        }
    } else {
        manifest.compactInstructionSize =
            static_cast<std::uint32_t>(sizeof(runtime::CompactInstruction));
    }

    const auto* nativeNode = root.find("nativeModuleAbi");
    if (!nativeNode || nativeNode->kind != MiniJsonValue::Kind::Object) {
        if (outError) *outError = "missing or invalid 'nativeModuleAbi' object";
        return std::nullopt;
    }
    if (!readU32Field(*nativeNode, "abiMajor", manifest.nativeModuleAbi.abiMajor, localErr) ||
        !readU32Field(*nativeNode, "abiMinor", manifest.nativeModuleAbi.abiMinor, localErr) ||
        !readU32Field(
            *nativeNode,
            "runtimeApiStructSize",
            manifest.nativeModuleAbi.runtimeApiStructSize,
            localErr) ||
        !readU32Field(
            *nativeNode,
            "functionEntryStructSize",
            manifest.nativeModuleAbi.functionEntryStructSize,
            localErr) ||
        !readU32Field(
            *nativeNode,
            "moduleExportsStructSize",
            manifest.nativeModuleAbi.moduleExportsStructSize,
            localErr)) {
        if (outError) *outError = localErr;
        return std::nullopt;
    }

    const auto* digestNode = root.find("layoutDigest");
    if (!digestNode) {
        if (outError) *outError = "missing 'layoutDigest' field";
        return std::nullopt;
    }
    if (digestNode->kind == MiniJsonValue::Kind::String) {
        if (!parseHexOrDecU64(digestNode->strVal, manifest.layoutDigest)) {
            if (outError) *outError = "invalid 'layoutDigest' hex/integer string";
            return std::nullopt;
        }
    } else if (digestNode->kind == MiniJsonValue::Kind::Number &&
               digestNode->hasExactUint) {
        manifest.layoutDigest = digestNode->uintVal;
    } else {
        if (outError) *outError = "invalid 'layoutDigest' value type";
        return std::nullopt;
    }

    return manifest;
}

bool AbiCompatibilityReport::hasIssue(AbiIssueCode expectedCode) const noexcept {
    return std::any_of(
        issues.begin(),
        issues.end(),
        [expectedCode](const AbiIssue& item) { return item.code == expectedCode; });
}

std::string AbiCompatibilityReport::summary() const {
    std::ostringstream out;
    out << abiCompatibilityStatusName(status);
    if (!issues.empty()) {
        out << " (" << issues.size() << " issue" << (issues.size() == 1 ? "" : "s")
            << "): ";
        for (std::size_t i = 0; i < issues.size(); ++i) {
            if (i != 0) out << "; ";
            out << "[" << issues[i].diagnosticCode << " "
                << abiIssueCodeName(issues[i].code) << "] " << issues[i].message;
        }
    }
    return out.str();
}

bool BytecodeHeaderCompatibilityReport::hasIssue(
    AbiIssueCode expectedCode) const noexcept {
    return std::any_of(
        issues.begin(),
        issues.end(),
        [expectedCode](const AbiIssue& item) { return item.code == expectedCode; });
}

const char* abiCompatibilityStatusName(AbiCompatibilityStatus status) noexcept {
    switch (status) {
    case AbiCompatibilityStatus::Compatible:
        return "Compatible";
    case AbiCompatibilityStatus::CompatibleWithMigration:
        return "CompatibleWithMigration";
    case AbiCompatibilityStatus::Incompatible:
        return "Incompatible";
    }
    return "Incompatible";
}

const char* abiIssueCodeName(AbiIssueCode code) noexcept {
    switch (code) {
    case AbiIssueCode::None:
        return "None";
    case AbiIssueCode::LayoutDigestMismatch:
        return "LayoutDigestMismatch";
    case AbiIssueCode::SchemaVersionMismatch:
        return "SchemaVersionMismatch";
    case AbiIssueCode::EngineMajorMismatch:
        return "EngineMajorMismatch";
    case AbiIssueCode::EngineMinorNewerThanHost:
        return "EngineMinorNewerThanHost";
    case AbiIssueCode::EngineMinorOlderMigration:
        return "EngineMinorOlderMigration";
    case AbiIssueCode::SdkCompatibilityMismatch:
        return "SdkCompatibilityMismatch";
    case AbiIssueCode::GameSdkPackageNewerThanHost:
        return "GameSdkPackageNewerThanHost";
    case AbiIssueCode::GameSdkPackageOlderMigration:
        return "GameSdkPackageOlderMigration";
    case AbiIssueCode::ScriptObjectStateNewerThanHost:
        return "ScriptObjectStateNewerThanHost";
    case AbiIssueCode::ScriptObjectStateMigrationRequired:
        return "ScriptObjectStateMigrationRequired";
    case AbiIssueCode::RsgsStateNewerThanHost:
        return "RsgsStateNewerThanHost";
    case AbiIssueCode::RsgsStateMigrationRequired:
        return "RsgsStateMigrationRequired";
    case AbiIssueCode::BytecodeVersionRangeInvalid:
        return "BytecodeVersionRangeInvalid";
    case AbiIssueCode::BytecodeVersionRangeDisjoint:
        return "BytecodeVersionRangeDisjoint";
    case AbiIssueCode::BytecodeTargetVersionUnsupported:
        return "BytecodeTargetVersionUnsupported";
    case AbiIssueCode::BytecodeLegacyMetadataMigration:
        return "BytecodeLegacyMetadataMigration";
    case AbiIssueCode::BytecodeHeaderTruncated:
        return "BytecodeHeaderTruncated";
    case AbiIssueCode::BytecodeInvalidMagic:
        return "BytecodeInvalidMagic";
    case AbiIssueCode::BytecodeSectionCountMismatch:
        return "BytecodeSectionCountMismatch";
    case AbiIssueCode::RuntimeValueLayoutMismatch:
        return "RuntimeValueLayoutMismatch";
    case AbiIssueCode::BytecodeInstructionLayoutMismatch:
        return "BytecodeInstructionLayoutMismatch";
    case AbiIssueCode::CompactInstructionLayoutMismatch:
        return "CompactInstructionLayoutMismatch";
    case AbiIssueCode::NativeModuleAbiMajorMismatch:
        return "NativeModuleAbiMajorMismatch";
    case AbiIssueCode::NativeModuleAbiMinorNewerThanHost:
        return "NativeModuleAbiMinorNewerThanHost";
    case AbiIssueCode::NativeModuleAbiMinorBackwardCompatible:
        return "NativeModuleAbiMinorBackwardCompatible";
    case AbiIssueCode::NativeModuleAbiStructSizeMismatch:
        return "NativeModuleAbiStructSizeMismatch";
    }
    return "Unknown";
}

AbiContractManifest currentAbiContractManifest() noexcept {
    static_assert(sizeof(runtime::Value) == 16, "runtime::Value ABI must be 16 bytes");
    static_assert(alignof(runtime::Value) == 16, "runtime::Value alignment must be 16 bytes");
    static_assert(
        sizeof(runtime::CompactInstruction) == 8,
        "runtime::CompactInstruction must be 8 bytes");
    static_assert(
        kNativeModuleAbiMajor == aot::RuntimeAbiMajor &&
            kNativeModuleAbiMinor == aot::RuntimeAbiMinor,
        "NativeModuleAbi constants must stay synchronized with AotRuntime");

    AbiContractManifest manifest{};
    manifest.schemaVersion = kAbiContractSchemaVersion;
    manifest.engineVersionMajor = kVersionMajor;
    manifest.engineVersionMinor = kVersionMinor;
    manifest.engineVersionPatch = kVersionPatch;
    manifest.sdkCompatibilityVersion = kSdkCompatibilityVersion;
    manifest.gameSdkPackageVersion = kGameSdkPackageVersion;
    manifest.scriptObjectStateVersion = kScriptObjectStateVersion;
    manifest.bytecodeMinVersion = BytecodeVersionPoint{
        kBytecodeMinSupportedMajor, kBytecodeMinSupportedMinor};
    manifest.bytecodeMaxVersion = BytecodeVersionPoint{
        kBytecodeMaxSupportedMajor, kBytecodeMaxSupportedMinor};
    manifest.bytecodeTargetVersion = BytecodeVersionPoint{
        kBytecodeDefaultMajor, kBytecodeDefaultMinor};
    manifest.rsgsStateVersion = kRsgsStateEncodingVersion;

    manifest.runtimeValueSize = static_cast<std::uint32_t>(sizeof(runtime::Value));
    manifest.runtimeValueAlign = static_cast<std::uint32_t>(alignof(runtime::Value));
    manifest.bytecodeInstructionSize =
        static_cast<std::uint32_t>(sizeof(bytecode::Instruction));
    manifest.bytecodeInstructionAlign =
        static_cast<std::uint32_t>(alignof(bytecode::Instruction));
    manifest.compactInstructionSize =
        static_cast<std::uint32_t>(sizeof(runtime::CompactInstruction));

    manifest.nativeModuleAbi.abiMajor = aot::RuntimeAbiMajor;
    manifest.nativeModuleAbi.abiMinor = aot::RuntimeAbiMinor;
    manifest.nativeModuleAbi.runtimeApiStructSize =
        static_cast<std::uint32_t>(sizeof(RsRuntimeApiV1));
    manifest.nativeModuleAbi.functionEntryStructSize =
        static_cast<std::uint32_t>(sizeof(RsFunctionEntryV1));
    manifest.nativeModuleAbi.moduleExportsStructSize =
        static_cast<std::uint32_t>(sizeof(RsModuleExportsV1));

    manifest.refreshLayoutDigest();
    return manifest;
}

AbiCompatibilityReport verifyAbiCompatibility(
    const AbiContractManifest& host,
    const AbiContractManifest& target) {
    AbiCompatibilityReport report{};

    if (!host.hasValidDigest()) {
        pushIssue(
            report,
            AbiIssueCode::LayoutDigestMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9001",
            "host.layoutDigest",
            "host ABI manifest layoutDigest is invalid or tampered (stored=" +
                formatHexU64(host.layoutDigest) + ", expected=" +
                formatHexU64(host.computeLayoutDigest()) + ")");
    }
    if (!target.hasValidDigest()) {
        pushIssue(
            report,
            AbiIssueCode::LayoutDigestMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9001",
            "target.layoutDigest",
            "target ABI manifest layoutDigest is invalid or tampered (stored=" +
                formatHexU64(target.layoutDigest) + ", expected=" +
                formatHexU64(target.computeLayoutDigest()) + ")");
    }

    if (host.schemaVersion != target.schemaVersion) {
        pushIssue(
            report,
            AbiIssueCode::SchemaVersionMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9002",
            "schemaVersion",
            "manifest schema version mismatch: host=" +
                std::to_string(host.schemaVersion) + ", target=" +
                std::to_string(target.schemaVersion));
    }

    if (host.runtimeValueSize != target.runtimeValueSize ||
        host.runtimeValueAlign != target.runtimeValueAlign) {
        pushIssue(
            report,
            AbiIssueCode::RuntimeValueLayoutMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9003",
            "runtimeValueLayout",
            "runtime::Value layout mismatch: host=" +
                std::to_string(host.runtimeValueSize) + "B/" +
                std::to_string(host.runtimeValueAlign) + "B, target=" +
                std::to_string(target.runtimeValueSize) + "B/" +
                std::to_string(target.runtimeValueAlign) + "B");
    }

    if (host.bytecodeInstructionSize != target.bytecodeInstructionSize ||
        host.bytecodeInstructionAlign != target.bytecodeInstructionAlign) {
        pushIssue(
            report,
            AbiIssueCode::BytecodeInstructionLayoutMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9004",
            "bytecodeInstructionLayout",
            "bytecode::Instruction layout mismatch: host=" +
                std::to_string(host.bytecodeInstructionSize) + "B/" +
                std::to_string(host.bytecodeInstructionAlign) + "B, target=" +
                std::to_string(target.bytecodeInstructionSize) + "B/" +
                std::to_string(target.bytecodeInstructionAlign) + "B");
    }

    if (host.compactInstructionSize != target.compactInstructionSize) {
        pushIssue(
            report,
            AbiIssueCode::CompactInstructionLayoutMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9005",
            "compactInstructionSize",
            "CompactInstruction size mismatch: host=" +
                std::to_string(host.compactInstructionSize) + "B, target=" +
                std::to_string(target.compactInstructionSize) + "B");
    }

    if (host.nativeModuleAbi.runtimeApiStructSize !=
            target.nativeModuleAbi.runtimeApiStructSize ||
        host.nativeModuleAbi.functionEntryStructSize !=
            target.nativeModuleAbi.functionEntryStructSize ||
        host.nativeModuleAbi.moduleExportsStructSize !=
            target.nativeModuleAbi.moduleExportsStructSize) {
        pushIssue(
            report,
            AbiIssueCode::NativeModuleAbiStructSizeMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9006",
            "nativeModuleAbi.structSizes",
            "C11 NativeModuleAbi struct sizes do not match host binary ABI");
    }

    if (host.nativeModuleAbi.abiMajor != target.nativeModuleAbi.abiMajor) {
        pushIssue(
            report,
            AbiIssueCode::NativeModuleAbiMajorMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9007",
            "nativeModuleAbi.abiMajor",
            "C11 NativeModuleAbi major version mismatch: host=" +
                std::to_string(host.nativeModuleAbi.abiMajor) + ", target=" +
                std::to_string(target.nativeModuleAbi.abiMajor));
    } else if (target.nativeModuleAbi.abiMinor > host.nativeModuleAbi.abiMinor) {
        pushIssue(
            report,
            AbiIssueCode::NativeModuleAbiMinorNewerThanHost,
            AbiCompatibilityStatus::Incompatible,
            "RS9008",
            "nativeModuleAbi.abiMinor",
            "target NativeModuleAbi minor version (" +
                std::to_string(target.nativeModuleAbi.abiMinor) +
                ") exceeds host (" +
                std::to_string(host.nativeModuleAbi.abiMinor) + ")");
    } else if (target.nativeModuleAbi.abiMinor < host.nativeModuleAbi.abiMinor) {
        pushIssue(
            report,
            AbiIssueCode::NativeModuleAbiMinorBackwardCompatible,
            AbiCompatibilityStatus::CompatibleWithMigration,
            "RS9009",
            "nativeModuleAbi.abiMinor",
            "target uses older backward-compatible NativeModuleAbi minor version");
    }

    if (host.sdkCompatibilityVersion != target.sdkCompatibilityVersion) {
        pushIssue(
            report,
            AbiIssueCode::SdkCompatibilityMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9010",
            "sdkCompatibilityVersion",
            "SDK compatibility version mismatch: host=" +
                std::to_string(host.sdkCompatibilityVersion) + ", target=" +
                std::to_string(target.sdkCompatibilityVersion));
    }

    if (target.gameSdkPackageVersion == 0 ||
        target.gameSdkPackageVersion > host.gameSdkPackageVersion) {
        pushIssue(
            report,
            AbiIssueCode::GameSdkPackageNewerThanHost,
            AbiCompatibilityStatus::Incompatible,
            "RS9011",
            "gameSdkPackageVersion",
            "Game SDK package version (" +
                std::to_string(target.gameSdkPackageVersion) +
                ") is unsupported by host (" +
                std::to_string(host.gameSdkPackageVersion) + ")");
    } else if (target.gameSdkPackageVersion < host.gameSdkPackageVersion) {
        pushIssue(
            report,
            AbiIssueCode::GameSdkPackageOlderMigration,
            AbiCompatibilityStatus::CompatibleWithMigration,
            "RS9012",
            "gameSdkPackageVersion",
            "legacy Game SDK package version requires loader migration");
    }

    if (target.scriptObjectStateVersion == 0 ||
        target.scriptObjectStateVersion > host.scriptObjectStateVersion) {
        pushIssue(
            report,
            AbiIssueCode::ScriptObjectStateNewerThanHost,
            AbiCompatibilityStatus::Incompatible,
            "RS9013",
            "scriptObjectStateVersion",
            "script object state version (" +
                std::to_string(target.scriptObjectStateVersion) +
                ") exceeds host (" +
                std::to_string(host.scriptObjectStateVersion) + ")");
    } else if (target.scriptObjectStateVersion < host.scriptObjectStateVersion) {
        pushIssue(
            report,
            AbiIssueCode::ScriptObjectStateMigrationRequired,
            AbiCompatibilityStatus::CompatibleWithMigration,
            "RS9014",
            "scriptObjectStateVersion",
            "older script object state version requires field schema migration");
    }

    if (target.rsgsStateVersion == 0 ||
        target.rsgsStateVersion > host.rsgsStateVersion) {
        pushIssue(
            report,
            AbiIssueCode::RsgsStateNewerThanHost,
            AbiCompatibilityStatus::Incompatible,
            "RS9015",
            "rsgsStateVersion",
            "RSGS state encoding version (" +
                std::to_string(target.rsgsStateVersion) +
                ") exceeds host (" + std::to_string(host.rsgsStateVersion) + ")");
    } else if (target.rsgsStateVersion < host.rsgsStateVersion) {
        pushIssue(
            report,
            AbiIssueCode::RsgsStateMigrationRequired,
            AbiCompatibilityStatus::CompatibleWithMigration,
            "RS9016",
            "rsgsStateVersion",
            "older RSGS state snapshot requires state codec migration");
    }

    if (host.bytecodeMinVersion > host.bytecodeMaxVersion ||
        target.bytecodeMinVersion > target.bytecodeMaxVersion ||
        host.bytecodeTargetVersion < host.bytecodeMinVersion ||
        host.bytecodeTargetVersion > host.bytecodeMaxVersion ||
        target.bytecodeTargetVersion < target.bytecodeMinVersion ||
        target.bytecodeTargetVersion > target.bytecodeMaxVersion) {
        pushIssue(
            report,
            AbiIssueCode::BytecodeVersionRangeInvalid,
            AbiCompatibilityStatus::Incompatible,
            "RS9017",
            "bytecodeVersionRange",
            "invalid .rsbc bytecode version interval or out-of-range target version");
    } else {
        const auto overlapMin =
            std::max(host.bytecodeMinVersion, target.bytecodeMinVersion);
        const auto overlapMax =
            std::min(host.bytecodeMaxVersion, target.bytecodeMaxVersion);
        if (overlapMin > overlapMax) {
            pushIssue(
                report,
                AbiIssueCode::BytecodeVersionRangeDisjoint,
                AbiCompatibilityStatus::Incompatible,
                "RS9018",
                "bytecodeVersionRange",
                "disjoint .rsbc bytecode support ranges: host=[" +
                    host.bytecodeMinVersion.toString() + ".." +
                    host.bytecodeMaxVersion.toString() + "], target=[" +
                    target.bytecodeMinVersion.toString() + ".." +
                    target.bytecodeMaxVersion.toString() + "]");
        }
        if (target.bytecodeTargetVersion < host.bytecodeMinVersion ||
            target.bytecodeTargetVersion > host.bytecodeMaxVersion) {
            pushIssue(
                report,
                AbiIssueCode::BytecodeTargetVersionUnsupported,
                AbiCompatibilityStatus::Incompatible,
                "RS9019",
                "bytecodeTargetVersion",
                "target .rsbc version " +
                    target.bytecodeTargetVersion.toString() +
                    " is outside host supported interval [" +
                    host.bytecodeMinVersion.toString() + ".." +
                    host.bytecodeMaxVersion.toString() + "]");
        } else if (target.bytecodeTargetVersion == BytecodeVersionPoint{0, 6} &&
                   host.bytecodeTargetVersion >= BytecodeVersionPoint{0, 7}) {
            pushIssue(
                report,
                AbiIssueCode::BytecodeLegacyMetadataMigration,
                AbiCompatibilityStatus::CompatibleWithMigration,
                "RS9020",
                "bytecodeTargetVersion",
                "legacy .rsbc 0.6 module requires object-model metadata rehydration");
        }
    }

    if (host.engineVersionMajor != target.engineVersionMajor) {
        pushIssue(
            report,
            AbiIssueCode::EngineMajorMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS9021",
            "engineVersionMajor",
            "engine major version mismatch: host=" +
                std::to_string(host.engineVersionMajor) + ", target=" +
                std::to_string(target.engineVersionMajor));
    } else if (target.engineVersionMinor > host.engineVersionMinor) {
        pushIssue(
            report,
            AbiIssueCode::EngineMinorNewerThanHost,
            AbiCompatibilityStatus::Incompatible,
            "RS9022",
            "engineVersionMinor",
            "target engine minor version (" +
                std::to_string(target.engineVersionMinor) +
                ") is newer than host (" +
                std::to_string(host.engineVersionMinor) + ")");
    } else if (target.engineVersionMinor < host.engineVersionMinor) {
        pushIssue(
            report,
            AbiIssueCode::EngineMinorOlderMigration,
            AbiCompatibilityStatus::CompatibleWithMigration,
            "RS9023",
            "engineVersionMinor",
            "target artifact was built with an older engine minor release");
    }

    return report;
}

BytecodeHeaderCompatibilityReport inspectBytecodeHeaderCompatibility(
    const std::uint8_t* data,
    std::size_t size,
    const AbiContractManifest& host) {
    BytecodeHeaderCompatibilityReport out{};
    if (data == nullptr || size < 16u) {
        out.status = AbiCompatibilityStatus::Incompatible;
        out.truncated = true;
        out.issues.push_back(AbiIssue{
            AbiIssueCode::BytecodeHeaderTruncated,
            AbiCompatibilityStatus::Incompatible,
            "RS5003",
            "rsbc.header",
            "truncated .rsbc header (requires at least 16 bytes, got " +
                std::to_string(size) + ")",
        });
        return out;
    }

    if (data[0] != 'R' || data[1] != 'S' || data[2] != 'B' || data[3] != 'C') {
        out.status = AbiCompatibilityStatus::Incompatible;
        out.validMagic = false;
        out.issues.push_back(AbiIssue{
            AbiIssueCode::BytecodeInvalidMagic,
            AbiCompatibilityStatus::Incompatible,
            "RS5000",
            "rsbc.magic",
            "invalid .rsbc magic header (expected 'RSBC')",
        });
        return out;
    }

    out.validMagic = true;
    out.version.major = readLeU16(data + 4);
    out.version.minor = readLeU16(data + 6);
    out.headerFlags = readLeU32(data + 8);
    out.sectionCount = readLeU32(data + 12);

    if (out.version < host.bytecodeMinVersion ||
        out.version > host.bytecodeMaxVersion ||
        out.version.major != 0) {
        out.status = AbiCompatibilityStatus::Incompatible;
        out.issues.push_back(AbiIssue{
            AbiIssueCode::BytecodeTargetVersionUnsupported,
            AbiCompatibilityStatus::Incompatible,
            "RS5001",
            "rsbc.version",
            "unsupported .rsbc bytecode version " + out.version.toString() +
                " (supported range [" + host.bytecodeMinVersion.toString() +
                ".." + host.bytecodeMaxVersion.toString() + "])",
        });
        return out;
    }

    out.hasObjectModelSection =
        (out.version.major == 0 && out.version.minor >= 7);
    out.hasSequenceResultMetadata =
        (out.version.major == 0 && out.version.minor >= 8);
    out.hasExceptionMetadata =
        (out.version.major == 0 && out.version.minor >= 9);
    out.expectedSectionCount = out.hasObjectModelSection ? 8u : 7u;

    if (out.sectionCount != out.expectedSectionCount) {
        out.status = AbiCompatibilityStatus::Incompatible;
        out.issues.push_back(AbiIssue{
            AbiIssueCode::BytecodeSectionCountMismatch,
            AbiCompatibilityStatus::Incompatible,
            "RS5002",
            "rsbc.sectionCount",
            "invalid .rsbc section count " + std::to_string(out.sectionCount) +
                " for version " + out.version.toString() + " (expected " +
                std::to_string(out.expectedSectionCount) + ")",
        });
        return out;
    }

    if (out.version == BytecodeVersionPoint{0, 6} &&
        host.bytecodeTargetVersion >= BytecodeVersionPoint{0, 7}) {
        out.status = AbiCompatibilityStatus::CompatibleWithMigration;
        out.issues.push_back(AbiIssue{
            AbiIssueCode::BytecodeLegacyMetadataMigration,
            AbiCompatibilityStatus::CompatibleWithMigration,
            "RS9020",
            "rsbc.version",
            "legacy .rsbc 0.6 header requires object-model metadata rehydration",
        });
        return out;
    }

    out.status = AbiCompatibilityStatus::Compatible;
    return out;
}

BytecodeHeaderCompatibilityReport inspectBytecodeHeaderCompatibility(
    const std::vector<std::uint8_t>& bytes,
    const AbiContractManifest& host) {
    return inspectBytecodeHeaderCompatibility(bytes.data(), bytes.size(), host);
}

} // namespace realscript
