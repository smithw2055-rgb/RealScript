#include "realscript/tooling/Json.h"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace realscript::tooling {
namespace {

const std::string emptyString;
const Json::Array emptyArray;
const Json::Object emptyObject;

void appendEscaped(std::ostringstream& output, const std::string& value) {
    output << '"';
    for (const auto character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(character))
                    << std::dec << std::setfill(' ');
            } else {
                output << character;
            }
            break;
        }
    }
    output << '"';
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    std::optional<Json> parse(std::string& error) {
        skip();
        auto value = parseValue(error, 0);
        if (!value) return std::nullopt;
        skip();
        if (position_ != text_.size()) {
            error = "unexpected trailing JSON content";
            return std::nullopt;
        }
        return value;
    }

private:
    void skip() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
    }

    std::optional<Json> parseValue(std::string& error, std::size_t depth) {
        constexpr std::size_t MaxJsonDepth = 256;
        if (depth > MaxJsonDepth) {
            error = "JSON nesting is too deep";
            return std::nullopt;
        }
        skip();
        if (position_ >= text_.size()) {
            error = "unexpected end of JSON";
            return std::nullopt;
        }
        switch (text_[position_]) {
        case 'n': return literal("null", Json(nullptr), error);
        case 't': return literal("true", Json(true), error);
        case 'f': return literal("false", Json(false), error);
        case '"': {
            auto string = parseString(error);
            return string ? std::optional<Json>(Json(std::move(*string))) : std::nullopt;
        }
        case '[': return parseArray(error, depth);
        case '{': return parseObject(error, depth);
        default:
            if (text_[position_] == '-' || std::isdigit(static_cast<unsigned char>(text_[position_]))) {
                return parseNumber(error);
            }
            error = "invalid JSON value";
            return std::nullopt;
        }
    }

    std::optional<Json> literal(
        const char* word,
        Json value,
        std::string& error) {
        const std::string expected(word);
        if (text_.compare(position_, expected.size(), expected) != 0) {
            error = "invalid JSON literal";
            return std::nullopt;
        }
        position_ += expected.size();
        return value;
    }

    std::optional<std::string> parseString(std::string& error) {
        if (text_[position_] != '"') return std::nullopt;
        ++position_;
        std::string output;
        while (position_ < text_.size()) {
            const auto character = text_[position_++];
            if (character == '"') return output;
            if (character != '\\') {
                if (static_cast<unsigned char>(character) < 0x20) {
                    error = "unescaped control character in JSON string";
                    return std::nullopt;
                }
                output.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) break;
            const auto escape = text_[position_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (position_ + 4 > text_.size()) {
                    error = "truncated JSON unicode escape";
                    return std::nullopt;
                }
                unsigned value = 0;
                for (int index = 0; index < 4; ++index) {
                    const auto digit = text_[position_++];
                    value *= 16;
                    if (digit >= '0' && digit <= '9') value += digit - '0';
                    else if (digit >= 'a' && digit <= 'f') value += digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') value += digit - 'A' + 10;
                    else {
                        error = "invalid JSON unicode escape";
                        return std::nullopt;
                    }
                }
                if (value <= 0x7f) output.push_back(static_cast<char>(value));
                else if (value <= 0x7ff) {
                    output.push_back(static_cast<char>(0xc0 | (value >> 6)));
                    output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
                } else {
                    output.push_back(static_cast<char>(0xe0 | (value >> 12)));
                    output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
                    output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
                }
                break;
            }
            default:
                error = "invalid JSON escape";
                return std::nullopt;
            }
        }
        error = "unterminated JSON string";
        return std::nullopt;
    }

    std::optional<Json> parseNumber(std::string& error) {
        const auto begin = position_;
        if (text_[position_] == '-') ++position_;
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
        }
        try {
            return Json(std::stod(text_.substr(begin, position_ - begin)));
        } catch (const std::exception&) {
            error = "invalid JSON number";
            return std::nullopt;
        }
    }

    std::optional<Json> parseArray(std::string& error, std::size_t depth) {
        ++position_;
        Json::Array values;
        skip();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return std::optional<Json>(std::in_place, std::move(values));
        }
        while (position_ < text_.size()) {
            auto value = parseValue(error, depth + 1);
            if (!value) return std::nullopt;
            values.push_back(std::move(*value));
            skip();
            if (position_ < text_.size() && text_[position_] == ']') {
                ++position_;
                return std::optional<Json>(std::in_place, std::move(values));
            }
            if (position_ >= text_.size() || text_[position_] != ',') {
                error = "expected comma in JSON array";
                return std::nullopt;
            }
            ++position_;
        }
        error = "unterminated JSON array";
        return std::nullopt;
    }

    std::optional<Json> parseObject(std::string& error, std::size_t depth) {
        ++position_;
        Json::Object values;
        skip();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return std::optional<Json>(std::in_place, std::move(values));
        }
        while (position_ < text_.size()) {
            skip();
            auto key = parseString(error);
            if (!key) return std::nullopt;
            skip();
            if (position_ >= text_.size() || text_[position_] != ':') {
                error = "expected colon in JSON object";
                return std::nullopt;
            }
            ++position_;
            auto value = parseValue(error, depth + 1);
            if (!value) return std::nullopt;
            if (!values.emplace(std::move(*key), std::move(*value)).second) {
                error = "duplicate JSON object key";
                return std::nullopt;
            }
            skip();
            if (position_ < text_.size() && text_[position_] == '}') {
                ++position_;
                return std::optional<Json>(std::in_place, std::move(values));
            }
            if (position_ >= text_.size() || text_[position_] != ',') {
                error = "expected comma in JSON object";
                return std::nullopt;
            }
            ++position_;
        }
        error = "unterminated JSON object";
        return std::nullopt;
    }

    const std::string& text_;
    std::size_t position_ = 0;
};

} // namespace

Json::Json(std::nullptr_t) noexcept : value_(nullptr) {}
Json::Json(bool value) noexcept : value_(value) {}
Json::Json(int value) noexcept : value_(static_cast<double>(value)) {}
Json::Json(std::int64_t value) noexcept : value_(static_cast<double>(value)) {}
Json::Json(double value) noexcept : value_(value) {}
Json::Json(std::string value)
    : value_(std::in_place_type<std::string>, std::move(value)) {}
Json::Json(const char* value) : value_(std::string(value ? value : "")) {}
Json::Json(Array value)
    : value_(std::in_place_type<Array>, std::move(value)) {}
Json::Json(Object value)
    : value_(std::in_place_type<Object>, std::move(value)) {}

bool Json::isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::isBool() const noexcept { return std::holds_alternative<bool>(value_); }
bool Json::isNumber() const noexcept { return std::holds_alternative<double>(value_); }
bool Json::isString() const noexcept { return std::holds_alternative<std::string>(value_); }
bool Json::isArray() const noexcept { return std::holds_alternative<Array>(value_); }
bool Json::isObject() const noexcept { return std::holds_alternative<Object>(value_); }

bool Json::boolValue(bool fallback) const noexcept {
    return isBool() ? std::get<bool>(value_) : fallback;
}
std::int64_t Json::integerValue(std::int64_t fallback) const noexcept {
    return isNumber() ? static_cast<std::int64_t>(std::get<double>(value_)) : fallback;
}
double Json::numberValue(double fallback) const noexcept {
    return isNumber() ? std::get<double>(value_) : fallback;
}
const std::string& Json::stringValue() const noexcept {
    return isString() ? std::get<std::string>(value_) : emptyString;
}
const Json::Array& Json::arrayValue() const noexcept {
    return isArray() ? std::get<Array>(value_) : emptyArray;
}
const Json::Object& Json::objectValue() const noexcept {
    return isObject() ? std::get<Object>(value_) : emptyObject;
}
Json::Array& Json::arrayValue() {
    if (!isArray()) value_ = Array{};
    return std::get<Array>(value_);
}
Json::Object& Json::objectValue() {
    if (!isObject()) value_ = Object{};
    return std::get<Object>(value_);
}
Json& Json::operator[](const std::string& key) { return objectValue()[key]; }
const Json* Json::find(const std::string& key) const noexcept {
    if (!isObject()) return nullptr;
    const auto found = std::get<Object>(value_).find(key);
    return found == std::get<Object>(value_).end() ? nullptr : &found->second;
}

std::string Json::dump() const {
    std::ostringstream output;
    if (isNull()) output << "null";
    else if (isBool()) output << (boolValue() ? "true" : "false");
    else if (isNumber()) {
        const auto number = numberValue();
        if (!std::isfinite(number)) {
            output << "null";
        } else if (std::floor(number) == number &&
                   number >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
                   number <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            output << static_cast<std::int64_t>(number);
        } else {
            output << std::setprecision(17) << number;
        }
    } else if (isString()) appendEscaped(output, stringValue());
    else if (isArray()) {
        output << '[';
        for (std::size_t index = 0; index < arrayValue().size(); ++index) {
            if (index != 0) output << ',';
            output << arrayValue()[index].dump();
        }
        output << ']';
    } else {
        output << '{';
        bool first = true;
        for (const auto& [key, value] : objectValue()) {
            if (!first) output << ',';
            first = false;
            appendEscaped(output, key);
            output << ':' << value.dump();
        }
        output << '}';
    }
    return output.str();
}

std::optional<Json> Json::parse(const std::string& text, std::string& error) {
    return Parser(text).parse(error);
}

bool readProtocolMessage(std::istream& input, std::string& body) {
    constexpr std::size_t MaxProtocolMessageSize = 16u * 1024u * 1024u;
    std::string line;
    std::size_t length = 0;
    bool foundLength = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const std::string prefix = "Content-Length:";
        if (line.rfind(prefix, 0) == 0) {
            try {
                const auto parsed = std::stoull(line.substr(prefix.size()));
                if (parsed > MaxProtocolMessageSize || foundLength) return false;
                length = static_cast<std::size_t>(parsed);
                foundLength = true;
            } catch (const std::exception&) {
                return false;
            }
        }
    }
    if (!foundLength) return false;
    body.assign(length, '\0');
    if (length == 0) return true;
    input.read(&body[0], static_cast<std::streamsize>(length));
    return input.good() || static_cast<std::size_t>(input.gcount()) == length;
}

void writeProtocolMessage(std::ostream& output, const Json& message) {
    const auto body = message.dump();
    output << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    output.flush();
}

std::string uriToPath(const std::string& uri) {
    const std::string prefix = "file://";
    return uri.rfind(prefix, 0) == 0 ? uri.substr(prefix.size()) : uri;
}

std::string pathToUri(const std::string& path) {
    return path.rfind("file://", 0) == 0 ? path : "file://" + path;
}

} // namespace realscript::tooling
