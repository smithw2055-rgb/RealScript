#pragma once

#include <cstdint>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace realscript::tooling {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) noexcept;
    Json(bool value) noexcept;
    Json(int value) noexcept;
    Json(std::int64_t value) noexcept;
    Json(double value) noexcept;
    Json(std::string value);
    Json(const char* value);
    Json(Array value);
    Json(Object value);

    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] bool isBool() const noexcept;
    [[nodiscard]] bool isNumber() const noexcept;
    [[nodiscard]] bool isString() const noexcept;
    [[nodiscard]] bool isArray() const noexcept;
    [[nodiscard]] bool isObject() const noexcept;

    [[nodiscard]] bool boolValue(bool fallback = false) const noexcept;
    [[nodiscard]] std::int64_t integerValue(std::int64_t fallback = 0) const noexcept;
    [[nodiscard]] double numberValue(double fallback = 0.0) const noexcept;
    [[nodiscard]] const std::string& stringValue() const noexcept;
    [[nodiscard]] const Array& arrayValue() const noexcept;
    [[nodiscard]] const Object& objectValue() const noexcept;
    [[nodiscard]] Array& arrayValue();
    [[nodiscard]] Object& objectValue();

    Json& operator[](const std::string& key);
    [[nodiscard]] const Json* find(const std::string& key) const noexcept;
    [[nodiscard]] std::string dump() const;

    [[nodiscard]] static std::optional<Json> parse(
        const std::string& text,
        std::string& error);

private:
    using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Value value_ = nullptr;
};

[[nodiscard]] bool readProtocolMessage(
    std::istream& input,
    std::string& body);
void writeProtocolMessage(
    std::ostream& output,
    const Json& message);

[[nodiscard]] std::string uriToPath(const std::string& uri);
[[nodiscard]] std::string pathToUri(const std::string& path);

} // namespace realscript::tooling
