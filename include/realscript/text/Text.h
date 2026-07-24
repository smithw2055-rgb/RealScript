#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realscript::text {

struct TextSpan {
    std::size_t start = 0;
    std::size_t length = 0;

    [[nodiscard]] constexpr std::size_t end() const noexcept { return start + length; }
    [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }

    [[nodiscard]] static constexpr TextSpan fromBounds(
        std::size_t begin,
        std::size_t endOffset) noexcept {
        return {begin, endOffset >= begin ? endOffset - begin : 0};
    }
};

struct LinePosition {
    std::size_t line = 0;
    std::size_t column = 0;
};

class SourceText {
public:
    explicit SourceText(std::string content, std::string name = "<memory>");

    [[nodiscard]] const std::string& content() const noexcept { return content_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::size_t size() const noexcept { return content_.size(); }
    [[nodiscard]] bool empty() const noexcept { return content_.empty(); }
    [[nodiscard]] char at(std::size_t offset) const noexcept;
    [[nodiscard]] std::string_view view(TextSpan span) const noexcept;
    [[nodiscard]] LinePosition linePosition(std::size_t offset) const noexcept;
    [[nodiscard]] std::size_t lineCount() const noexcept { return lineStarts_.size(); }

private:
    void buildLineMap();

    std::string content_;
    std::string name_;
    std::vector<std::size_t> lineStarts_;
};

} // namespace realscript::text
