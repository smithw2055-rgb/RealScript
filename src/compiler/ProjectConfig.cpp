#include "realscript/compiler/ProjectConfig.h"

#include "realscript/CompilerPluginAbi.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace realscript::compiler {
namespace {

struct LocalJsonValue {
    struct Null {};
    struct Number {};
    using Array = std::vector<LocalJsonValue>;
    using Object = std::vector<std::pair<std::string, LocalJsonValue>>;
    std::variant<Null, bool, Number, std::string, Array, Object> data{Null{}};

    [[nodiscard]] bool isObject() const noexcept {
        return std::holds_alternative<Object>(data);
    }
    [[nodiscard]] bool isArray() const noexcept {
        return std::holds_alternative<Array>(data);
    }
    [[nodiscard]] bool isString() const noexcept {
        return std::holds_alternative<std::string>(data);
    }
    [[nodiscard]] const Array& arrayValue() const noexcept {
        return std::get<Array>(data);
    }
    [[nodiscard]] const std::string& stringValue() const noexcept {
        return std::get<std::string>(data);
    }
    [[nodiscard]] const LocalJsonValue* find(std::string_view key) const noexcept {
        if (!isObject()) return nullptr;
        const auto& entries = std::get<Object>(data);
        for (const auto& entry : entries) {
            if (entry.first == key) return &entry.second;
        }
        return nullptr;
    }
};

class LocalJsonParser {
public:
    explicit LocalJsonParser(std::string_view input) noexcept : input_(input) {}

    [[nodiscard]] std::optional<LocalJsonValue> parse(std::string& error) {
        if (input_.size() >= 3 &&
            static_cast<unsigned char>(input_[0]) == 0xEF &&
            static_cast<unsigned char>(input_[1]) == 0xBB &&
            static_cast<unsigned char>(input_[2]) == 0xBF) {
            pos_ = 3;
        }
        skipWs();
        auto root = parseValue(0, error);
        if (!root.has_value()) {
            return std::nullopt;
        }
        skipWs();
        if (pos_ != input_.size()) {
            error = "unexpected trailing characters in JSON";
            return std::nullopt;
        }
        return root;
    }

private:
    static constexpr std::size_t kMaxDepth = 128;

    void skipWs() noexcept {
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    static bool appendUtf8(std::uint32_t cp, std::string& out) {
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
            return true;
        }
        if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            return true;
        }
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            return true;
        }
        if (cp <= 0x10FFFFu) {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            return true;
        }
        return false;
    }

    bool parseHex4(std::uint32_t& out) noexcept {
        if (pos_ + 4 > input_.size()) return false;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[pos_++];
            std::uint32_t d = 0;
            if (c >= '0' && c <= '9') d = static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
            v = (v << 4) | d;
        }
        out = v;
        return true;
    }

    std::optional<std::string> parseString(std::string& error) {
        if (pos_ >= input_.size() || input_[pos_] != '"') {
            error = "expected '\"'";
            return std::nullopt;
        }
        ++pos_;
        std::string out;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20u) {
                error = "unescaped control character in JSON string";
                return std::nullopt;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= input_.size()) {
                error = "unterminated escape in JSON string";
                return std::nullopt;
            }
            const char esc = input_[pos_++];
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
                    std::uint32_t u1 = 0;
                    if (!parseHex4(u1)) {
                        error = "invalid unicode escape in JSON string";
                        return std::nullopt;
                    }
                    std::uint32_t cp = u1;
                    if (u1 >= 0xD800u && u1 <= 0xDBFFu) {
                        if (pos_ + 2 <= input_.size() &&
                            input_[pos_] == '\\' && input_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            std::uint32_t u2 = 0;
                            if (!parseHex4(u2) || u2 < 0xDC00u || u2 > 0xDFFFu) {
                                error = "invalid UTF-16 surrogate pair in JSON string";
                                return std::nullopt;
                            }
                            cp = 0x10000u + (((u1 - 0xD800u) << 10) | (u2 - 0xDC00u));
                        } else {
                            error = "unpaired high surrogate in JSON string";
                            return std::nullopt;
                        }
                    } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
                        error = "unexpected low surrogate in JSON string";
                        return std::nullopt;
                    }
                    if (!appendUtf8(cp, out)) {
                        error = "invalid unicode code point in JSON string";
                        return std::nullopt;
                    }
                    break;
                }
                default:
                    error = "invalid escape sequence in JSON string";
                    return std::nullopt;
            }
        }
        error = "unterminated JSON string";
        return std::nullopt;
    }

    bool skipNumber(std::string& error) noexcept {
        if (pos_ < input_.size() && input_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= input_.size()) {
            error = "invalid JSON number";
            return false;
        }
        if (input_[pos_] == '0') {
            ++pos_;
        } else if (input_[pos_] >= '1' && input_[pos_] <= '9') {
            while (pos_ < input_.size() &&
                   input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        } else {
            error = "invalid JSON number";
            return false;
        }
        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
                error = "invalid fractional JSON number";
                return false;
            }
            while (pos_ < input_.size() &&
                   input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
                error = "invalid exponent in JSON number";
                return false;
            }
            while (pos_ < input_.size() &&
                   input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        }
        return true;
    }

    std::optional<LocalJsonValue> parseValue(std::size_t depth, std::string& error) {
        if (depth > kMaxDepth) {
            error = "JSON nesting depth limit exceeded";
            return std::nullopt;
        }
        skipWs();
        if (pos_ >= input_.size()) {
            error = "unexpected end of JSON";
            return std::nullopt;
        }
        const char c = input_[pos_];
        if (c == '{') {
            return parseObject(depth, error);
        }
        if (c == '[') {
            return parseArray(depth, error);
        }
        if (c == '"') {
            auto s = parseString(error);
            if (!s.has_value()) return std::nullopt;
            LocalJsonValue v;
            v.data = std::move(*s);
            return v;
        }
        if (c == 't' && input_.substr(pos_, 4) == "true") {
            pos_ += 4;
            LocalJsonValue v;
            v.data = true;
            return v;
        }
        if (c == 'f' && input_.substr(pos_, 5) == "false") {
            pos_ += 5;
            LocalJsonValue v;
            v.data = false;
            return v;
        }
        if (c == 'n' && input_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return LocalJsonValue{};
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            if (!skipNumber(error)) return std::nullopt;
            LocalJsonValue v;
            v.data = LocalJsonValue::Number{};
            return v;
        }
        error = "unexpected token in JSON";
        return std::nullopt;
    }

    std::optional<LocalJsonValue> parseArray(std::size_t depth, std::string& error) {
        ++pos_;
        skipWs();
        LocalJsonValue::Array items;
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            LocalJsonValue v;
            v.data = std::move(items);
            return v;
        }
        while (true) {
            auto elem = parseValue(depth + 1, error);
            if (!elem.has_value()) return std::nullopt;
            items.push_back(std::move(*elem));
            skipWs();
            if (pos_ < input_.size() && input_[pos_] == ',') {
                ++pos_;
                skipWs();
                continue;
            }
            if (pos_ < input_.size() && input_[pos_] == ']') {
                ++pos_;
                LocalJsonValue v;
                v.data = std::move(items);
                return v;
            }
            error = "expected ',' or ']' in JSON array";
            return std::nullopt;
        }
    }

    std::optional<LocalJsonValue> parseObject(std::size_t depth, std::string& error) {
        ++pos_;
        skipWs();
        LocalJsonValue::Object entries;
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            LocalJsonValue v;
            v.data = std::move(entries);
            return v;
        }
        while (true) {
            skipWs();
            auto key = parseString(error);
            if (!key.has_value()) return std::nullopt;
            skipWs();
            if (pos_ >= input_.size() || input_[pos_] != ':') {
                error = "expected ':' after JSON object key";
                return std::nullopt;
            }
            ++pos_;
            auto val = parseValue(depth + 1, error);
            if (!val.has_value()) return std::nullopt;
            bool replaced = false;
            for (auto& existing : entries) {
                if (existing.first == *key) {
                    existing.second = std::move(*val);
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                entries.emplace_back(std::move(*key), std::move(*val));
            }
            skipWs();
            if (pos_ < input_.size() && input_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < input_.size() && input_[pos_] == '}') {
                ++pos_;
                LocalJsonValue v;
                v.data = std::move(entries);
                return v;
            }
            error = "expected ',' or '}' in JSON object";
            return std::nullopt;
        }
    }

    std::string_view input_;
    std::size_t pos_ = 0;
};

[[nodiscard]] std::optional<LocalJsonValue> parseProjectJson(
    std::string_view text,
    std::string& error) {
    LocalJsonParser parser(text);
    return parser.parse(error);
}

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

[[nodiscard]] std::uint64_t fnvMixBytes(
    std::uint64_t hash,
    const void* data,
    std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t fnvMixU64(std::uint64_t hash, std::uint64_t v) noexcept {
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
        hash ^= static_cast<std::uint8_t>((v >> shift) & 0xFFu);
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t fnvMixString(std::uint64_t hash, std::string_view s) noexcept {
    hash = fnvMixU64(hash, static_cast<std::uint64_t>(s.size()));
    return fnvMixBytes(hash, s.data(), s.size());
}

[[nodiscard]] std::uint64_t combineFp(std::uint64_t a, std::uint64_t b) noexcept {
    return fnvMixU64(a == 0 ? kFnvOffsetBasis : a, b);
}

[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto& wide = path.native();
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return path.string();
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    return out;
#else
    return path.string();
#endif
}

[[nodiscard]] std::filesystem::path utf8ToPath(std::string_view utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        nullptr, 0);
    if (needed <= 0) {
        return std::filesystem::path(std::string(utf8));
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        wide.data(), needed);
    return std::filesystem::path(std::move(wide));
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

[[nodiscard]] bool readBinaryFile(
    const std::filesystem::path& path,
    std::string& outBytes,
    std::string& outError) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        outError = "unable to open file '" + pathToUtf8(path) + "'";
        return false;
    }
    const auto endPos = in.tellg();
    if (endPos < 0) {
        outError = "unable to determine file size for '" + pathToUtf8(path) + "'";
        return false;
    }
    outBytes.resize(static_cast<std::size_t>(endPos));
    in.seekg(0, std::ios::beg);
    if (!outBytes.empty() &&
        !in.read(outBytes.data(), static_cast<std::streamsize>(outBytes.size()))) {
        outError = "failed to read file '" + pathToUtf8(path) + "'";
        outBytes.clear();
        return false;
    }
    return true;
}

[[nodiscard]] bool isVirtualSourceName(std::string_view name) noexcept {
    if (name.empty()) return true;
    return name.front() == '<' ||
        name.front() == '$' ||
        name.rfind("inmemory://", 0) == 0 ||
        name.rfind("untitled:", 0) == 0;
}

[[nodiscard]] std::filesystem::path normalizeResolvedPath(
    const std::filesystem::path& baseDir,
    std::string_view raw) {
    auto p = utf8ToPath(raw);
    if (p.is_relative() && !baseDir.empty()) {
        p = baseDir / p;
    }
    std::error_code ec;
    auto abs = std::filesystem::weakly_canonical(p, ec);
    if (!ec) return abs;
    return p.lexically_normal();
}

[[nodiscard]] std::filesystem::path resolvePluginPathCandidate(
    const std::filesystem::path& baseDir,
    std::string_view raw) {
    auto resolved = normalizeResolvedPath(baseDir, raw);
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec) && !resolved.has_extension()) {
#ifdef _WIN32
        constexpr const char* kExt = ".dll";
#elif defined(__APPLE__)
        constexpr const char* kExt = ".dylib";
#else
        constexpr const char* kExt = ".so";
#endif
        auto withExt = resolved;
        withExt += kExt;
        if (std::filesystem::exists(withExt, ec)) {
            return withExt;
        }
    }
    return resolved;
}

[[nodiscard]] std::uint64_t hashPathAndContents(
    std::uint64_t hash,
    const std::filesystem::path& path,
    std::string_view tag) {
    hash = fnvMixString(hash, tag);
    hash = fnvMixString(hash, pathToUtf8(path));
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        const auto sz = std::filesystem::file_size(path, ec);
        hash = fnvMixU64(hash, ec ? 0ull : static_cast<std::uint64_t>(sz));
        const auto mtime = std::filesystem::last_write_time(path, ec);
        hash = fnvMixU64(
            hash,
            ec ? 0ull : static_cast<std::uint64_t>(mtime.time_since_epoch().count()));
        std::string bytes;
        std::string err;
        if (readBinaryFile(path, bytes, err)) {
            hash = fnvMixString(hash, bytes);
        }
    } else {
        hash = fnvMixString(hash, "missing");
    }
    return hash;
}

[[nodiscard]] std::string stripAttributeSuffix(std::string_view name) {
    constexpr std::string_view kSuffix = "Attribute";
    if (name.size() > kSuffix.size() &&
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        return std::string(name.substr(0, name.size() - kSuffix.size()));
    }
    return std::string(name);
}

[[nodiscard]] std::string trimAscii(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

[[nodiscard]] std::string sanitizeId(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalnum(u) != 0 || ch == '_' || ch == '-') {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? std::string("plugin") : out;
}

struct MacroHostExpandState {
    diagnostics::DiagnosticBag* diagnostics = nullptr;
    std::vector<std::string> generatedSources;
    std::string currentSourcePath;
    text::TextSpan currentSpan;
    std::string pluginName;
};

extern "C" void rsProjectMacroSetGeneratedSource(
    void* host_ctx,
    const char* utf8_source,
    size_t byte_len) {
    auto* state = static_cast<MacroHostExpandState*>(host_ctx);
    if (!state || !utf8_source || byte_len == 0) {
        return;
    }
    state->generatedSources.emplace_back(utf8_source, byte_len);
}

extern "C" void rsProjectMacroReportDiagnostic(
    void* host_ctx,
    const char* code_utf8,
    const char* message_utf8,
    uint8_t severity) {
    auto* state = static_cast<MacroHostExpandState*>(host_ctx);
    if (!state || !state->diagnostics) {
        return;
    }
    std::string code = (code_utf8 && *code_utf8) ? std::string(code_utf8) : std::string("RS7110");
    std::string message = (message_utf8 && *message_utf8)
        ? std::string(message_utf8)
        : std::string("macro plugin '" + state->pluginName + "' reported a diagnostic");
    const auto sev = (severity == RS_COMPILER_DIAG_WARNING_V1)
        ? diagnostics::DiagnosticSeverity::Warning
        : diagnostics::DiagnosticSeverity::Error;
    state->diagnostics->report(
        std::move(code),
        std::move(message),
        state->currentSpan,
        sev,
        state->currentSourcePath);
}

class NativeCompilerMacroPlugin final : public ICompilerMacroPlugin {
public:
    NativeCompilerMacroPlugin(
#ifdef _WIN32
        HMODULE handle,
#else
        void* handle,
#endif
        const RsCompilerMacroPluginV1* table,
        std::uint64_t pluginFingerprint)
        : handle_(handle),
          table_(table),
          pluginName_(table->plugin_name ? table->plugin_name : ""),
          pluginVersion_(table->plugin_version ? table->plugin_version : ""),
          fingerprint_(pluginFingerprint) {
        macroNames_.reserve(table_->macro_count);
        for (std::size_t i = 0; i < table_->macro_count; ++i) {
            const auto& desc = table_->macros[i];
            if (!desc.macro_name || !*desc.macro_name) continue;
            std::string mname(desc.macro_name);
            macroNames_.push_back(mname);
            if (desc.kind == RS_COMPILER_MACRO_ATTRIBUTE_V1) {
                attributeMacros_.emplace(stripAttributeSuffix(mname), std::move(mname));
            } else if (desc.kind == RS_COMPILER_MACRO_EXPRESSION_V1) {
                expressionMacros_.emplace(mname, std::move(mname));
            }
        }
    }

    ~NativeCompilerMacroPlugin() override {
        if (table_ && table_->cleanup) {
            table_->cleanup(table_->plugin_user_data);
        }
#ifdef _WIN32
        if (handle_) {
            FreeLibrary(handle_);
            handle_ = nullptr;
        }
#else
        if (handle_) {
            dlclose(handle_);
            handle_ = nullptr;
        }
#endif
    }

    NativeCompilerMacroPlugin(const NativeCompilerMacroPlugin&) = delete;
    NativeCompilerMacroPlugin& operator=(const NativeCompilerMacroPlugin&) = delete;

    [[nodiscard]] std::string name() const override { return pluginName_; }
    [[nodiscard]] std::vector<std::string> macroNames() const override { return macroNames_; }
    [[nodiscard]] std::uint64_t fingerprint() const override { return fingerprint_; }

    void expand(
        const MacroExpansionContext& context,
        std::vector<SourceFile>& outGeneratedSources,
        diagnostics::DiagnosticBag& diagnostics) const override {
        if (!table_ || !table_->expand) return;

        MacroHostExpandState hostState;
        hostState.diagnostics = &diagnostics;
        hostState.pluginName = pluginName_;

        RsCompilerMacroHostV1 hostVtable{};
        hostVtable.set_generated_source = &rsProjectMacroSetGeneratedSource;
        hostVtable.report_diagnostic = &rsProjectMacroReportDiagnostic;

        std::lock_guard<std::mutex> lock(expandMutex_);

        auto invokeMacro = [&](const std::string& registeredName,
                               std::uint8_t kind,
                               const MacroAstUnitView& unit,
                               const std::string& targetName,
                               const std::string& targetDeclUtf8,
                               const std::string& argsUtf8,
                               std::vector<std::string>& argNameStorage,
                               std::vector<bool>& argHasName,
                               std::vector<std::string>& argValueStorage,
                               text::TextSpan siteSpan) {
            std::vector<const char*> argNamesPtrs;
            std::vector<const char*> argValuesPtrs;
            argNamesPtrs.reserve(argValueStorage.size());
            argValuesPtrs.reserve(argValueStorage.size());
            for (std::size_t i = 0; i < argValueStorage.size(); ++i) {
                argNamesPtrs.push_back(argHasName[i] ? argNameStorage[i].c_str() : nullptr);
                argValuesPtrs.push_back(argValueStorage[i].c_str());
            }
            const auto pos = unit.sourceText->linePosition(siteSpan.start);
            RsCompilerMacroExpandInputV1 input{};
            input.macro_name = registeredName.c_str();
            input.kind = kind;
            input.module_name = unit.moduleName.empty() ? nullptr : unit.moduleName.c_str();
            input.source_path = unit.sourcePath.c_str();
            input.target_name = targetName.empty() ? nullptr : targetName.c_str();
            input.target_declaration_utf8 = targetDeclUtf8.c_str();
            input.arguments_utf8 = argsUtf8.c_str();
            input.arg_count = static_cast<std::uint32_t>(argValuesPtrs.size());
            input.arg_names = argNamesPtrs.empty() ? nullptr : argNamesPtrs.data();
            input.arg_values = argValuesPtrs.empty() ? nullptr : argValuesPtrs.data();
            input.line = static_cast<std::uint32_t>(pos.line + 1);
            input.column = static_cast<std::uint32_t>(pos.column + 1);

            hostState.currentSourcePath = unit.sourcePath;
            hostState.currentSpan = siteSpan;
            table_->expand(table_->plugin_user_data, &input, &hostVtable, &hostState);
        };

        auto emitAttrMacros = [&](const MacroAstUnitView& unit,
                                  const std::vector<syntax::AttributeListSyntax>& lists,
                                  const std::string& targetName,
                                  text::TextSpan declSpan) {
            if (attributeMacros_.empty() || lists.empty()) return;
            const std::string declText(unit.sourceText->view(declSpan));
            for (const auto& list : lists) {
                for (const auto& attr : list.attributes) {
                    const auto shortName = stripAttributeSuffix(attr.nameToken.text);
                    auto macroIt = attributeMacros_.find(shortName);
                    if (macroIt == attributeMacros_.end()) {
                        macroIt = attributeMacros_.find(attr.nameToken.text);
                    }
                    if (macroIt == attributeMacros_.end()) continue;

                    std::string rawArgs;
                    if (attr.openParenToken && attr.closeParenToken &&
                        attr.closeParenToken->span.start >= attr.openParenToken->span.end()) {
                        rawArgs = trimAscii(unit.sourceText->view(text::TextSpan::fromBounds(
                            attr.openParenToken->span.end(),
                            attr.closeParenToken->span.start)));
                    }
                    std::vector<std::string> argNameStorage;
                    std::vector<bool> argHasName;
                    std::vector<std::string> argValueStorage;
                    argNameStorage.reserve(attr.arguments.size());
                    argHasName.reserve(attr.arguments.size());
                    argValueStorage.reserve(attr.arguments.size());
                    for (const auto& arg : attr.arguments) {
                        if (arg.nameToken) {
                            argHasName.push_back(true);
                            argNameStorage.push_back(arg.nameToken->text);
                        } else {
                            argHasName.push_back(false);
                            argNameStorage.emplace_back();
                        }
                        if (arg.valueTokens.size() == 1 &&
                            arg.valueTokens[0].kind == syntax::SyntaxKind::StringLiteralToken &&
                            std::holds_alternative<std::string>(arg.valueTokens[0].value)) {
                            argValueStorage.push_back(std::get<std::string>(arg.valueTokens[0].value));
                        } else {
                            argValueStorage.push_back(trimAscii(unit.sourceText->view(arg.valueSpan())));
                        }
                    }
                    invokeMacro(
                        macroIt->second,
                        RS_COMPILER_MACRO_ATTRIBUTE_V1,
                        unit,
                        targetName,
                        declText,
                        rawArgs,
                        argNameStorage,
                        argHasName,
                        argValueStorage,
                        attr.span());
                }
            }
        };

        auto scanCallOrExprArgs = [&](const MacroAstUnitView& unit,
                                      const syntax::CallExpressionSyntax& call,
                                      const std::string& targetName) {
            const auto macroIt = expressionMacros_.find(call.identifierToken.text);
            if (macroIt == expressionMacros_.end()) return;
            const auto callSpan = call.span();
            std::string rawArgs;
            if (call.closeParenToken.span.start >= call.openParenToken.span.end()) {
                rawArgs = trimAscii(unit.sourceText->view(text::TextSpan::fromBounds(
                    call.openParenToken.span.end(),
                    call.closeParenToken.span.start)));
            }
            std::vector<std::string> argNameStorage;
            std::vector<bool> argHasName;
            std::vector<std::string> argValueStorage;
            argNameStorage.reserve(call.arguments.size());
            argHasName.reserve(call.arguments.size());
            argValueStorage.reserve(call.arguments.size());
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                if (i < call.argumentNames.size() && call.argumentNames[i].has_value()) {
                    argHasName.push_back(true);
                    argNameStorage.push_back(call.argumentNames[i]->text);
                } else {
                    argHasName.push_back(false);
                    argNameStorage.emplace_back();
                }
                if (call.arguments[i]) {
                    if (call.arguments[i]->kind() == syntax::SyntaxKind::LiteralExpression) {
                        const auto& lit = static_cast<const syntax::LiteralExpressionSyntax&>(*call.arguments[i]);
                        if (lit.literalToken.kind == syntax::SyntaxKind::StringLiteralToken &&
                            std::holds_alternative<std::string>(lit.literalToken.value)) {
                            argValueStorage.push_back(std::get<std::string>(lit.literalToken.value));
                            continue;
                        }
                    }
                    argValueStorage.push_back(trimAscii(unit.sourceText->view(call.arguments[i]->span())));
                } else {
                    argValueStorage.emplace_back();
                }
            }
            invokeMacro(
                macroIt->second,
                RS_COMPILER_MACRO_EXPRESSION_V1,
                unit,
                targetName,
                std::string(unit.sourceText->view(callSpan)),
                rawArgs,
                argNameStorage,
                argHasName,
                argValueStorage,
                callSpan);
        };

        for (const auto& unit : context.parsedUnits) {
            if (!unit.syntaxTree || !unit.sourceText) continue;

            std::function<void(const syntax::PatternSyntax*, const std::string&)> visitPattern;
            std::function<void(const syntax::ExpressionSyntax*, const std::string&)> visitExpr;
            std::function<void(const syntax::StatementSyntax*, const std::string&)> visitStmt;

            visitPattern = [&](const syntax::PatternSyntax* pat, const std::string& ownerName) {
                if (!pat) return;
                switch (pat->kind()) {
                case syntax::SyntaxKind::ConstantPattern: {
                    const auto& cp = static_cast<const syntax::ConstantPatternSyntax&>(*pat);
                    visitExpr(cp.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::RelationalPattern: {
                    const auto& rp = static_cast<const syntax::RelationalPatternSyntax&>(*pat);
                    visitExpr(rp.operand.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::UnaryPattern: {
                    const auto& up = static_cast<const syntax::UnaryPatternSyntax&>(*pat);
                    visitPattern(up.pattern.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::BinaryPattern: {
                    const auto& bp = static_cast<const syntax::BinaryPatternSyntax&>(*pat);
                    visitPattern(bp.left.get(), ownerName);
                    visitPattern(bp.right.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::PropertyPattern: {
                    const auto& pp = static_cast<const syntax::PropertyPatternSyntax&>(*pat);
                    for (const auto& sub : pp.properties) {
                        visitPattern(sub.pattern.get(), ownerName);
                    }
                    break;
                }
                case syntax::SyntaxKind::ParenthesizedPattern: {
                    const auto& pp = static_cast<const syntax::ParenthesizedPatternSyntax&>(*pat);
                    visitPattern(pp.pattern.get(), ownerName);
                    break;
                }
                default:
                    break;
                }
            };

            visitExpr = [&](const syntax::ExpressionSyntax* expr, const std::string& ownerName) {
                if (!expr) return;
                switch (expr->kind()) {
                case syntax::SyntaxKind::TaggedStringExpression: {
                    const auto& ts = static_cast<const syntax::TaggedStringExpressionSyntax&>(*expr);
                    const auto macroIt = expressionMacros_.find(ts.tagToken.text);
                    if (macroIt != expressionMacros_.end()) {
                        std::string litVal;
                        if (std::holds_alternative<std::string>(ts.stringToken.value)) {
                            litVal = std::get<std::string>(ts.stringToken.value);
                        } else {
                            litVal = std::string(unit.sourceText->view(ts.stringToken.span));
                        }
                        const auto sp = ts.span();
                        std::vector<std::string> names = {std::string()};
                        std::vector<bool> hasNames = {false};
                        std::vector<std::string> values = {litVal};
                        invokeMacro(
                            macroIt->second,
                            RS_COMPILER_MACRO_EXPRESSION_V1,
                            unit,
                            ownerName,
                            std::string(unit.sourceText->view(sp)),
                            std::string(unit.sourceText->view(ts.stringToken.span)),
                            names,
                            hasNames,
                            values,
                            sp);
                    }
                    break;
                }
                case syntax::SyntaxKind::CallExpression: {
                    const auto& call = static_cast<const syntax::CallExpressionSyntax&>(*expr);
                    scanCallOrExprArgs(unit, call, ownerName);
                    for (const auto& arg : call.arguments) {
                        visitExpr(arg.get(), ownerName);
                    }
                    break;
                }
                case syntax::SyntaxKind::MemberCallExpression: {
                    const auto& mc = static_cast<const syntax::MemberCallExpressionSyntax&>(*expr);
                    visitExpr(mc.receiver.get(), ownerName);
                    for (const auto& arg : mc.arguments) {
                        visitExpr(arg.get(), ownerName);
                    }
                    break;
                }
                case syntax::SyntaxKind::LambdaExpression: {
                    const auto& lm = static_cast<const syntax::LambdaExpressionSyntax&>(*expr);
                    visitExpr(lm.body.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::UnaryExpression: {
                    const auto& ue = static_cast<const syntax::UnaryExpressionSyntax&>(*expr);
                    visitExpr(ue.operand.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::BinaryExpression: {
                    const auto& be = static_cast<const syntax::BinaryExpressionSyntax&>(*expr);
                    visitExpr(be.left.get(), ownerName);
                    visitExpr(be.right.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::TypeBinaryExpression: {
                    const auto& tb = static_cast<const syntax::TypeBinaryExpressionSyntax&>(*expr);
                    visitExpr(tb.expression.get(), ownerName);
                    visitPattern(tb.pattern.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::SwitchExpression: {
                    const auto& se = static_cast<const syntax::SwitchExpressionSyntax&>(*expr);
                    visitExpr(se.expression.get(), ownerName);
                    for (const auto& arm : se.arms) {
                        visitPattern(arm.pattern.get(), ownerName);
                        visitExpr(arm.label.get(), ownerName);
                        visitExpr(arm.guard.get(), ownerName);
                        visitExpr(arm.value.get(), ownerName);
                    }
                    break;
                }
                case syntax::SyntaxKind::ConditionalExpression: {
                    const auto& ce = static_cast<const syntax::ConditionalExpressionSyntax&>(*expr);
                    visitExpr(ce.condition.get(), ownerName);
                    visitExpr(ce.whenTrue.get(), ownerName);
                    visitExpr(ce.whenFalse.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::AssignmentExpression: {
                    const auto& ae = static_cast<const syntax::AssignmentExpressionSyntax&>(*expr);
                    visitExpr(ae.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::ParenthesizedExpression: {
                    const auto& pe = static_cast<const syntax::ParenthesizedExpressionSyntax&>(*expr);
                    visitExpr(pe.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::CastExpression: {
                    const auto& ce = static_cast<const syntax::CastExpressionSyntax&>(*expr);
                    visitExpr(ce.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::MemberAccessExpression: {
                    const auto& ma = static_cast<const syntax::MemberAccessExpressionSyntax&>(*expr);
                    visitExpr(ma.receiver.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::ElementAccessExpression: {
                    const auto& ea = static_cast<const syntax::ElementAccessExpressionSyntax&>(*expr);
                    visitExpr(ea.receiver.get(), ownerName);
                    visitExpr(ea.index.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::ElementAssignmentExpression: {
                    const auto& ea = static_cast<const syntax::ElementAssignmentExpressionSyntax&>(*expr);
                    visitExpr(ea.receiver.get(), ownerName);
                    visitExpr(ea.index.get(), ownerName);
                    visitExpr(ea.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::MemberAssignmentExpression: {
                    const auto& ma = static_cast<const syntax::MemberAssignmentExpressionSyntax&>(*expr);
                    visitExpr(ma.receiver.get(), ownerName);
                    visitExpr(ma.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::NewObjectExpression: {
                    const auto& ne = static_cast<const syntax::NewObjectExpressionSyntax&>(*expr);
                    for (const auto& arg : ne.arguments) {
                        visitExpr(arg.get(), ownerName);
                    }
                    for (const auto& initEl : ne.initializers) {
                        for (const auto& sub : initEl.expressions) {
                            visitExpr(sub.get(), ownerName);
                        }
                    }
                    break;
                }
                case syntax::SyntaxKind::NewArrayExpression: {
                    const auto& na = static_cast<const syntax::NewArrayExpressionSyntax&>(*expr);
                    visitExpr(na.length.get(), ownerName);
                    for (const auto& iv : na.initialValues) {
                        visitExpr(iv.get(), ownerName);
                    }
                    break;
                }
                default:
                    break;
                }
            };

            visitStmt = [&](const syntax::StatementSyntax* stmt, const std::string& ownerName) {
                if (!stmt || expressionMacros_.empty()) return;
                switch (stmt->kind()) {
                case syntax::SyntaxKind::BlockStatement: {
                    const auto& blk = static_cast<const syntax::BlockStatementSyntax&>(*stmt);
                    for (const auto& s : blk.statements) visitStmt(s.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::ExpressionStatement: {
                    const auto& es = static_cast<const syntax::ExpressionStatementSyntax&>(*stmt);
                    visitExpr(es.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::VariableDeclarationStatement: {
                    const auto& vd = static_cast<const syntax::VariableDeclarationStatementSyntax&>(*stmt);
                    visitExpr(vd.initializer.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::ReturnStatement: {
                    const auto& rs = static_cast<const syntax::ReturnStatementSyntax&>(*stmt);
                    visitExpr(rs.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::IfStatement: {
                    const auto& ifs = static_cast<const syntax::IfStatementSyntax&>(*stmt);
                    visitExpr(ifs.condition.get(), ownerName);
                    visitStmt(ifs.thenStatement.get(), ownerName);
                    visitStmt(ifs.elseStatement.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::WhileStatement: {
                    const auto& ws = static_cast<const syntax::WhileStatementSyntax&>(*stmt);
                    visitExpr(ws.condition.get(), ownerName);
                    visitStmt(ws.body.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::ForStatement: {
                    const auto& fs = static_cast<const syntax::ForStatementSyntax&>(*stmt);
                    visitStmt(fs.initializer.get(), ownerName);
                    visitExpr(fs.condition.get(), ownerName);
                    visitExpr(fs.increment.get(), ownerName);
                    visitStmt(fs.body.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::ForeachStatement: {
                    const auto& fe = static_cast<const syntax::ForeachStatementSyntax&>(*stmt);
                    visitExpr(fe.collection.get(), ownerName);
                    visitStmt(fe.body.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::DoWhileStatement: {
                    const auto& dw = static_cast<const syntax::DoWhileStatementSyntax&>(*stmt);
                    visitStmt(dw.body.get(), ownerName);
                    visitExpr(dw.condition.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::SwitchStatement: {
                    const auto& ss = static_cast<const syntax::SwitchStatementSyntax&>(*stmt);
                    visitExpr(ss.expression.get(), ownerName);
                    for (const auto& sec : ss.sections) {
                        visitPattern(sec.pattern.get(), ownerName);
                        visitExpr(sec.label.get(), ownerName);
                        visitExpr(sec.guard.get(), ownerName);
                        for (const auto& child : sec.statements) visitStmt(child.get(), ownerName);
                    }
                    break;
                }
                case syntax::SyntaxKind::ThrowStatement: {
                    const auto& ts = static_cast<const syntax::ThrowStatementSyntax&>(*stmt);
                    visitExpr(ts.expression.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::YieldWaitStatement: {
                    const auto& yw = static_cast<const syntax::YieldWaitStatementSyntax&>(*stmt);
                    visitExpr(yw.delay.get(), ownerName);
                    visitExpr(yw.secondArgument.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::EventSubscriptionStatement: {
                    const auto& ev = static_cast<const syntax::EventSubscriptionStatementSyntax&>(*stmt);
                    visitExpr(ev.receiver.get(), ownerName);
                    visitExpr(ev.handler.get(), ownerName);
                    break;
                }
                case syntax::SyntaxKind::TryStatement: {
                    const auto& tr = static_cast<const syntax::TryStatementSyntax&>(*stmt);
                    visitStmt(&tr.body, ownerName);
                    for (const auto& c : tr.catches) visitStmt(&c.body, ownerName);
                    if (tr.finallyBody) visitStmt(&*tr.finallyBody, ownerName);
                    break;
                }
                default:
                    break;
                }
            };

            const auto& cu = *unit.syntaxTree;
            if (cu.moduleDeclaration) {
                emitAttrMacros(
                    unit,
                    cu.moduleDeclaration->attributes,
                    cu.moduleDeclaration->fullName(),
                    cu.moduleDeclaration->span());
            }

            const auto visitRecord = [&](const auto& decls) {
                for (const auto& rec : decls) {
                    emitAttrMacros(unit, rec.attributes, rec.identifierToken.text, rec.span());
                    for (const auto& fld : rec.fields) {
                        emitAttrMacros(unit, fld.attributes, fld.identifierToken.text, fld.span());
                        visitExpr(fld.initializer.get(), fld.identifierToken.text);
                    }
                    for (const auto& ev : rec.events) {
                        emitAttrMacros(unit, ev.attributes, ev.identifierToken.text, ev.span());
                    }
                    for (const auto& prop : rec.properties) {
                        emitAttrMacros(unit, prop.attributes, prop.identifierToken.text, prop.span());
                        if (prop.getter && prop.getter->body) {
                            visitStmt(prop.getter->body.get(), prop.identifierToken.text);
                        }
                        if (prop.setter && prop.setter->body) {
                            visitStmt(prop.setter->body.get(), prop.identifierToken.text);
                        }
                        visitExpr(prop.initializer.get(), prop.identifierToken.text);
                    }
                    for (const auto& ctor : rec.constructors) {
                        emitAttrMacros(unit, ctor.attributes, ctor.identifierToken.text, ctor.span());
                        for (const auto& p : ctor.parameters) {
                            emitAttrMacros(unit, p.attributes, p.identifierToken.text, p.span());
                            visitExpr(p.defaultValue.get(), p.identifierToken.text);
                        }
                        for (const auto& ba : ctor.baseArguments) {
                            visitExpr(ba.get(), ctor.identifierToken.text);
                        }
                        visitStmt(&ctor.body, ctor.identifierToken.text);
                    }
                    for (const auto& m : rec.methods) {
                        emitAttrMacros(unit, m.attributes, m.identifierToken.text, m.span());
                        for (const auto& p : m.parameters) {
                            emitAttrMacros(unit, p.attributes, p.identifierToken.text, p.span());
                            visitExpr(p.defaultValue.get(), p.identifierToken.text);
                        }
                        if (m.hasBody()) {
                            visitStmt(&m.body, m.identifierToken.text);
                        }
                    }
                    for (const auto& s : rec.sequences) {
                        emitAttrMacros(unit, s.attributes, s.identifierToken.text, s.span());
                        for (const auto& p : s.parameters) {
                            emitAttrMacros(unit, p.attributes, p.identifierToken.text, p.span());
                            visitExpr(p.defaultValue.get(), p.identifierToken.text);
                        }
                        visitStmt(&s.body, s.identifierToken.text);
                    }
                }
            };
            visitRecord(cu.classes);
            visitRecord(cu.structs);

            for (const auto& en : cu.enums) {
                emitAttrMacros(unit, en.attributes, en.identifierToken.text, en.span());
                for (const auto& em : en.members) {
                    emitAttrMacros(unit, em.attributes, em.identifierToken.text, em.span());
                }
            }
            for (const auto& iface : cu.interfaces) {
                emitAttrMacros(unit, iface.attributes, iface.identifierToken.text, iface.span());
                for (const auto& m : iface.methods) {
                    emitAttrMacros(unit, m.attributes, m.identifierToken.text, m.span());
                    for (const auto& p : m.parameters) {
                        emitAttrMacros(unit, p.attributes, p.identifierToken.text, p.span());
                        visitExpr(p.defaultValue.get(), p.identifierToken.text);
                    }
                    if (m.hasBody() && m.body) {
                        visitStmt(&*m.body, m.identifierToken.text);
                    }
                }
            }
            for (const auto& del : cu.delegates) {
                emitAttrMacros(unit, del.attributes, del.identifierToken.text, del.span());
                for (const auto& p : del.parameters) {
                    emitAttrMacros(unit, p.attributes, p.identifierToken.text, p.span());
                    visitExpr(p.defaultValue.get(), p.identifierToken.text);
                }
            }
            for (const auto& fn : cu.functions) {
                emitAttrMacros(unit, fn.attributes, fn.identifierToken.text, fn.span());
                for (const auto& p : fn.parameters) {
                    emitAttrMacros(unit, p.attributes, p.identifierToken.text, p.span());
                    visitExpr(p.defaultValue.get(), p.identifierToken.text);
                }
                if (fn.hasBody()) {
                    visitStmt(&fn.body, fn.identifierToken.text);
                }
            }
        }

        const std::string cleanPlugin = sanitizeId(pluginName_);
        for (std::size_t i = 0; i < hostState.generatedSources.size(); ++i) {
            outGeneratedSources.push_back(SourceFile{
                "$generated/plugin/" + cleanPlugin + "/" + std::to_string(i) + ".g.rscript",
                std::move(hostState.generatedSources[i])});
        }
    }

private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
    const RsCompilerMacroPluginV1* table_ = nullptr;
    std::string pluginName_;
    std::string pluginVersion_;
    std::uint64_t fingerprint_ = 0;
    std::vector<std::string> macroNames_;
    std::unordered_map<std::string, std::string> attributeMacros_;
    std::unordered_map<std::string, std::string> expressionMacros_;
    mutable std::mutex expandMutex_;
};

thread_local const CompilationOptions* tlsScopedProjectOptions = nullptr;

} // namespace

std::optional<std::filesystem::path> discoverProjectConfigFile(
    const std::filesystem::path& startPathOrDir) {
    std::error_code ec;
    std::filesystem::path start = startPathOrDir;
    if (start.empty()) {
        start = std::filesystem::current_path(ec);
        if (ec || start.empty()) return std::nullopt;
    }

    if (start.filename() == "realscript.project.json" &&
        std::filesystem::is_regular_file(start, ec)) {
        auto abs = std::filesystem::weakly_canonical(start, ec);
        return ec ? start.lexically_normal() : abs;
    }

    std::filesystem::path dir = start;
    if (!std::filesystem::is_directory(dir, ec)) {
        dir = start.parent_path();
        if (dir.empty()) {
            dir = std::filesystem::current_path(ec);
        }
    }
    if (dir.empty()) return std::nullopt;
    if (dir.is_relative()) {
        auto absDir = std::filesystem::absolute(dir, ec);
        if (!ec) dir = std::move(absDir);
    }

    while (!dir.empty()) {
        const auto candidate = dir / "realscript.project.json";
        if (std::filesystem::is_regular_file(candidate, ec)) {
            auto abs = std::filesystem::weakly_canonical(candidate, ec);
            return ec ? candidate.lexically_normal() : abs;
        }
        const auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> discoverProjectConfigFile(
    const std::vector<std::filesystem::path>& hintPaths) {
    for (const auto& hint : hintPaths) {
        if (hint.empty()) continue;
        if (auto found = discoverProjectConfigFile(hint)) {
            return found;
        }
    }
    return discoverProjectConfigFile(std::filesystem::path{});
}

std::optional<ProjectConfig> loadProjectConfigFile(
    const std::filesystem::path& configPath,
    diagnostics::DiagnosticBag* diagnostics) {
    std::error_code ec;
    auto normalizedPath = std::filesystem::weakly_canonical(configPath, ec);
    if (ec) {
        normalizedPath = configPath.lexically_normal();
    }
    const std::string pathUtf8 = pathToUtf8(normalizedPath);

    std::string jsonText;
    std::string readErr;
    if (!readBinaryFile(normalizedPath, jsonText, readErr)) {
        if (diagnostics) {
            diagnostics->report(
                "RS7101",
                "failed to read project config '" + pathUtf8 + "': " + readErr,
                {},
                diagnostics::DiagnosticSeverity::Error,
                pathUtf8);
        }
        return std::nullopt;
    }

    std::string parseErr;
    const auto parsed = parseProjectJson(jsonText, parseErr);
    if (!parsed.has_value() || !parsed->isObject()) {
        if (diagnostics) {
            diagnostics->report(
                "RS7101",
                "invalid project config JSON in '" + pathUtf8 + "'" +
                    (parseErr.empty() ? "" : (": " + parseErr)),
                {},
                diagnostics::DiagnosticSeverity::Error,
                pathUtf8);
        }
        return std::nullopt;
    }

    ProjectConfig config;
    config.configPath = normalizedPath;
    const auto baseDir = normalizedPath.parent_path();

    auto parseStringArray = [&](const char* key,
                                std::vector<std::string>& outValues) -> bool {
        const auto* field = parsed->find(key);
        if (!field) return true;
        if (!field->isArray()) {
            if (diagnostics) {
                diagnostics->report(
                    "RS7101",
                    std::string("project config field '") + key + "' must be an array",
                    {},
                    diagnostics::DiagnosticSeverity::Error,
                    pathUtf8);
            }
            return false;
        }
        for (const auto& item : field->arrayValue()) {
            if (!item.isString() || item.stringValue().empty()) {
                if (diagnostics) {
                    diagnostics->report(
                        "RS7101",
                        std::string("project config field '") + key + "' entries must be non-empty strings",
                        {},
                        diagnostics::DiagnosticSeverity::Error,
                        pathUtf8);
                }
                return false;
            }
            outValues.push_back(item.stringValue());
        }
        return true;
    };

    std::vector<std::string> disabledList;
    if (!parseStringArray("disabledPacks", disabledList) ||
        !parseStringArray("disabledBuiltinPacks", disabledList)) {
        return std::nullopt;
    }
    for (auto& d : disabledList) {
        config.disabledBuiltinPacks.insert(std::move(d));
    }

    std::vector<std::string> extraPackStrings;
    if (!parseStringArray("extraPacks", extraPackStrings)) {
        return std::nullopt;
    }
    for (const auto& raw : extraPackStrings) {
        config.extraPackFiles.push_back(normalizeResolvedPath(baseDir, raw));
    }

    if (const auto* macroField = parsed->find("macroPlugins")) {
        if (!macroField->isArray()) {
            if (diagnostics) {
                diagnostics->report(
                    "RS7101",
                    "project config field 'macroPlugins' must be an array",
                    {},
                    diagnostics::DiagnosticSeverity::Error,
                    pathUtf8);
            }
            return std::nullopt;
        }
        for (const auto& item : macroField->arrayValue()) {
            std::string rawPath;
            if (item.isString()) {
                rawPath = item.stringValue();
            } else if (item.isObject()) {
                if (const auto* pField = item.find("path"); pField && pField->isString()) {
                    rawPath = pField->stringValue();
                }
            }
            if (rawPath.empty()) {
                if (diagnostics) {
                    diagnostics->report(
                        "RS7101",
                        "project config 'macroPlugins' entries must be non-empty library paths",
                        {},
                        diagnostics::DiagnosticSeverity::Error,
                        pathUtf8);
                }
                return std::nullopt;
            }
            config.nativeMacroPluginPaths.push_back(
                resolvePluginPathCandidate(baseDir, rawPath));
        }
    }

    std::uint64_t fp = kFnvOffsetBasis;
    fp = fnvMixString(fp, "project_v1");
    fp = fnvMixString(fp, pathUtf8);
    fp = fnvMixString(fp, jsonText);

    std::vector<std::string> sortedDisabled(
        config.disabledBuiltinPacks.begin(), config.disabledBuiltinPacks.end());
    std::sort(sortedDisabled.begin(), sortedDisabled.end());
    for (const auto& d : sortedDisabled) {
        fp = fnvMixString(fp, "disabled");
        fp = fnvMixString(fp, d);
    }

    auto sortedExtra = config.extraPackFiles;
    std::sort(sortedExtra.begin(), sortedExtra.end(), [](const auto& a, const auto& b) {
        return pathToUtf8(a) < pathToUtf8(b);
    });
    for (const auto& p : sortedExtra) {
        fp = hashPathAndContents(fp, p, "pack");
    }

    auto sortedPlugins = config.nativeMacroPluginPaths;
    std::sort(sortedPlugins.begin(), sortedPlugins.end(), [](const auto& a, const auto& b) {
        return pathToUtf8(a) < pathToUtf8(b);
    });
    for (const auto& p : sortedPlugins) {
        fp = fnvMixU64(fp, RS_COMPILER_PLUGIN_ABI_VERSION_1);
        fp = hashPathAndContents(fp, p, "plugin");
    }

    config.configFingerprint = fp == 0 ? 1ull : fp;
    return config;
}

std::shared_ptr<const ICompilerMacroPlugin> loadNativeCompilerMacroPlugin(
    const std::filesystem::path& libraryPath,
    diagnostics::DiagnosticBag* diagnostics) {
    const std::string libUtf8 = pathToUtf8(libraryPath);
    if (libraryPath.empty()) {
        if (diagnostics) {
            diagnostics->report(
                "RS7103",
                "compiler macro plugin path is empty",
                {},
                diagnostics::DiagnosticSeverity::Error,
                libUtf8);
        }
        return nullptr;
    }

#ifdef _WIN32
    HMODULE handle = LoadLibraryW(libraryPath.c_str());
    if (!handle) {
        if (diagnostics) {
            diagnostics->report(
                "RS7103",
                "failed to load compiler macro plugin library '" + libUtf8 +
                    "' (Win32 error " + std::to_string(GetLastError()) + ")",
                {},
                diagnostics::DiagnosticSeverity::Error,
                libUtf8);
        }
        return nullptr;
    }
    const auto rawSym = GetProcAddress(handle, RS_COMPILER_MACRO_PLUGIN_ENTRY_V1);
    if (!rawSym) {
        FreeLibrary(handle);
        if (diagnostics) {
            diagnostics->report(
                "RS7103",
                "macro plugin library '" + libUtf8 + "' is missing entry point '" +
                    RS_COMPILER_MACRO_PLUGIN_ENTRY_V1 + "'",
                {},
                diagnostics::DiagnosticSeverity::Error,
                libUtf8);
        }
        return nullptr;
    }
    const auto entryFn = reinterpret_cast<RsGetCompilerMacroPluginV1Fn>(
        reinterpret_cast<void*>(rawSym));
#else
    void* handle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* dlErr = dlerror();
        if (diagnostics) {
            diagnostics->report(
                "RS7103",
                "failed to load compiler macro plugin library '" + libUtf8 + "'" +
                    (dlErr ? (std::string(": ") + dlErr) : std::string()),
                {},
                diagnostics::DiagnosticSeverity::Error,
                libUtf8);
        }
        return nullptr;
    }
    const auto rawSym = dlsym(handle, RS_COMPILER_MACRO_PLUGIN_ENTRY_V1);
    if (!rawSym) {
        dlclose(handle);
        if (diagnostics) {
            diagnostics->report(
                "RS7103",
                "macro plugin library '" + libUtf8 + "' is missing entry point '" +
                    RS_COMPILER_MACRO_PLUGIN_ENTRY_V1 + "'",
                {},
                diagnostics::DiagnosticSeverity::Error,
                libUtf8);
        }
        return nullptr;
    }
    const auto entryFn = reinterpret_cast<RsGetCompilerMacroPluginV1Fn>(rawSym);
#endif

    auto closeOnFailure = [&]() {
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
    };

    const RsCompilerMacroPluginV1* table = entryFn ? entryFn() : nullptr;
    if (!table ||
        table->struct_size < sizeof(RsCompilerMacroPluginV1) ||
        table->abi_version != RS_COMPILER_PLUGIN_ABI_VERSION_1 ||
        !table->plugin_name ||
        !*table->plugin_name ||
        !table->expand ||
        (table->macro_count > 0 && !table->macros)) {
        closeOnFailure();
        if (diagnostics) {
            diagnostics->report(
                "RS7103",
                "invalid compiler macro plugin ABI descriptor or incompatible ABI version in '" +
                    libUtf8 + "'",
                {},
                diagnostics::DiagnosticSeverity::Error,
                libUtf8);
        }
        return nullptr;
    }

    for (std::size_t i = 0; i < table->macro_count; ++i) {
        const auto& desc = table->macros[i];
        if (!desc.macro_name || !*desc.macro_name ||
            (desc.kind != RS_COMPILER_MACRO_ATTRIBUTE_V1 &&
             desc.kind != RS_COMPILER_MACRO_EXPRESSION_V1)) {
            closeOnFailure();
            if (diagnostics) {
                diagnostics->report(
                    "RS7103",
                    "invalid macro descriptor entry in plugin '" + libUtf8 + "'",
                    {},
                    diagnostics::DiagnosticSeverity::Error,
                    libUtf8);
            }
            return nullptr;
        }
    }

    std::uint64_t pluginFp = kFnvOffsetBasis;
    pluginFp = hashPathAndContents(pluginFp, libraryPath, "native_plugin");
    pluginFp = fnvMixU64(pluginFp, table->abi_version);
    pluginFp = fnvMixString(pluginFp, table->plugin_name);
    if (table->plugin_version) {
        pluginFp = fnvMixString(pluginFp, table->plugin_version);
    }
    for (std::size_t i = 0; i < table->macro_count; ++i) {
        pluginFp = fnvMixString(pluginFp, table->macros[i].macro_name);
        pluginFp = fnvMixU64(pluginFp, table->macros[i].kind);
    }

    return std::make_shared<NativeCompilerMacroPlugin>(
        handle, table, pluginFp == 0 ? 1ull : pluginFp);
}

bool applyProjectConfigToOptions(
    const ProjectConfig& config,
    CompilationOptions& options,
    diagnostics::DiagnosticBag* diagnostics) {
    for (const auto& disabled : config.disabledBuiltinPacks) {
        options.disabledPacks.insert(disabled);
    }

    for (const auto& packPath : config.extraPackFiles) {
        const std::string packUtf8 = pathToUtf8(packPath);
        std::string content;
        std::string readErr;
        if (!readBinaryFile(packPath, content, readErr)) {
            if (diagnostics) {
                diagnostics->report(
                    "RS7102",
                    "failed to load extra module pack '" + packUtf8 + "': " + readErr,
                    {},
                    diagnostics::DiagnosticSeverity::Error,
                    packUtf8);
            }
            return false;
        }

        std::string moduleName;
        std::string effectiveSource = content;
        std::string virtualPath = packUtf8;

        if (packPath.extension() == ".json") {
            std::string jsonErr;
            const auto doc = parseProjectJson(content, jsonErr);
            if (!doc.has_value() || !doc->isObject()) {
                if (diagnostics) {
                    diagnostics->report(
                        "RS7102",
                        "invalid JSON pack descriptor '" + packUtf8 + "'",
                        {},
                        diagnostics::DiagnosticSeverity::Error,
                        packUtf8);
                }
                return false;
            }
            if (const auto* mn = doc->find("moduleName"); mn && mn->isString()) {
                moduleName = mn->stringValue();
            }
            if (const auto* srcField = doc->find("source"); srcField && srcField->isString()) {
                effectiveSource = srcField->stringValue();
            } else if (const auto* fileField = doc->find("path"); fileField && fileField->isString()) {
                const auto subPath = normalizeResolvedPath(packPath.parent_path(), fileField->stringValue());
                virtualPath = pathToUtf8(subPath);
                if (!readBinaryFile(subPath, effectiveSource, readErr)) {
                    if (diagnostics) {
                        diagnostics->report(
                            "RS7102",
                            "failed to read pack source '" + virtualPath + "': " + readErr,
                            {},
                            diagnostics::DiagnosticSeverity::Error,
                            virtualPath);
                    }
                    return false;
                }
            }
        }

        if (moduleName.empty()) {
            text::SourceText src(effectiveSource, virtualPath);
            diagnostics::DiagnosticBag parseDiags;
            syntax::Parser parser(src, parseDiags);
            const auto cu = parser.parseCompilationUnit();
            if (cu.moduleDeclaration) {
                moduleName = cu.moduleDeclaration->fullName();
            }
        }
        if (moduleName.empty()) {
            if (diagnostics) {
                diagnostics->report(
                    "RS7102",
                    "extra module pack '" + packUtf8 + "' does not declare a 'module Name;' header",
                    {},
                    diagnostics::DiagnosticSeverity::Error,
                    packUtf8);
            }
            return false;
        }

        ModulePack pack;
        pack.moduleName = moduleName;
        pack.virtualSourcePath = virtualPath;
        auto sharedSource = std::make_shared<std::string>(std::move(effectiveSource));
        pack.sourceProvider = [sharedSource]() -> const std::string& { return *sharedSource; };
        const auto existingIt = std::find_if(
            options.extraPacks.begin(),
            options.extraPacks.end(),
            [&](const ModulePack& existing) { return existing.moduleName == moduleName; });
        if (existingIt != options.extraPacks.end()) {
            *existingIt = std::move(pack);
        } else {
            options.extraPacks.push_back(std::move(pack));
        }
    }

    for (const auto& pluginPath : config.nativeMacroPluginPaths) {
        auto plugin = loadNativeCompilerMacroPlugin(pluginPath, diagnostics);
        if (!plugin) {
            return false;
        }
        options.macroPlugins.push_back(std::move(plugin));
    }

    options.projectConfigFingerprint = combineFp(
        options.projectConfigFingerprint,
        config.configFingerprint);
    options.projectConfigPath = pathToUtf8(config.configPath);
    return true;
}

bool resolveAndApplyProjectConfigForInputs(
    const std::vector<SourceFile>& sources,
    const std::optional<std::filesystem::path>& explicitConfigPath,
    bool noProjectConfig,
    CompilationOptions& options,
    diagnostics::DiagnosticBag* diagnostics) {
    if (noProjectConfig) {
        return true;
    }
    std::optional<std::filesystem::path> configPath;
    if (explicitConfigPath.has_value()) {
        std::error_code ec;
        if (std::filesystem::is_directory(*explicitConfigPath, ec)) {
            configPath = discoverProjectConfigFile(*explicitConfigPath);
        } else {
            configPath = *explicitConfigPath;
        }
        if (!configPath.has_value()) {
            if (diagnostics) {
                diagnostics->report(
                    "RS7101",
                    "could not locate 'realscript.project.json' under '" +
                        pathToUtf8(*explicitConfigPath) + "'",
                    {},
                    diagnostics::DiagnosticSeverity::Error,
                    pathToUtf8(*explicitConfigPath));
            }
            return false;
        }
    } else {
        std::vector<std::filesystem::path> hints;
        hints.reserve(sources.size());
        for (const auto& src : sources) {
            if (!isVirtualSourceName(src.path)) {
                hints.push_back(utf8ToPath(src.path));
            }
        }
        configPath = discoverProjectConfigFile(hints);
    }
    if (!configPath.has_value()) {
        return true;
    }
    const auto cfg = loadProjectConfigFile(*configPath, diagnostics);
    if (!cfg.has_value()) {
        return false;
    }
    return applyProjectConfigToOptions(*cfg, options, diagnostics);
}

ScopedProjectCompilationOptions::ScopedProjectCompilationOptions(
    const CompilationOptions& options) noexcept
    : previous_(tlsScopedProjectOptions), active_(options) {
    tlsScopedProjectOptions = &active_;
}

ScopedProjectCompilationOptions::~ScopedProjectCompilationOptions() {
    tlsScopedProjectOptions = previous_;
}

const CompilationOptions* ScopedProjectCompilationOptions::current() noexcept {
    return tlsScopedProjectOptions;
}

} // namespace realscript::compiler
