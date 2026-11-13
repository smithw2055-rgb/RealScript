#include "realscript/text/Text.h"

#include <algorithm>

namespace realscript::text {

SourceText::SourceText(std::string content, std::string name)
    : content_(std::move(content)), name_(std::move(name)) {
    buildLineMap();
}

char SourceText::at(std::size_t offset) const noexcept {
    return offset < content_.size() ? content_[offset] : '\0';
}

std::string_view SourceText::view(TextSpan span) const noexcept {
    if (span.start >= content_.size()) {
        return {};
    }
    const auto safeLength = std::min(span.length, content_.size() - span.start);
    return std::string_view(content_).substr(span.start, safeLength);
}

LinePosition SourceText::linePosition(std::size_t offset) const noexcept {
    offset = std::min(offset, content_.size());
    const auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    const auto line = it == lineStarts_.begin()
        ? std::size_t{0}
        : static_cast<std::size_t>(std::distance(lineStarts_.begin(), it) - 1);
    return {line, offset - lineStarts_[line]};
}

void SourceText::buildLineMap() {
    lineStarts_.clear();
    lineStarts_.push_back(0);

    for (std::size_t i = 0; i < content_.size(); ++i) {
        if (content_[i] == '\r') {
            if (i + 1 < content_.size() && content_[i + 1] == '\n') {
                ++i;
            }
            lineStarts_.push_back(i + 1);
        } else if (content_[i] == '\n') {
            lineStarts_.push_back(i + 1);
        }
    }
}

namespace {

[[nodiscard]] constexpr bool isUtf8Continuation(unsigned char b) noexcept {
    return (b & 0xC0u) == 0x80u;
}

// Decodes one UTF-8 character starting at `idx` in `text`, returning {byteLength, utf16Units}.
// Malformed sequences advance 1 byte and count as 1 UTF-16 unit so callers never stall.
[[nodiscard]] constexpr std::pair<std::size_t, std::size_t> decodeUtf8Step(
    std::string_view text,
    std::size_t idx) noexcept {
    const unsigned char b0 = static_cast<unsigned char>(text[idx]);
    if (b0 < 0x80u) {
        return {1u, 1u};
    }
    if ((b0 & 0xE0u) == 0xC0u) {
        if (idx + 1u < text.size() &&
            isUtf8Continuation(static_cast<unsigned char>(text[idx + 1u]))) {
            return {2u, 1u};
        }
        return {1u, 1u};
    }
    if ((b0 & 0xF0u) == 0xE0u) {
        if (idx + 2u < text.size() &&
            isUtf8Continuation(static_cast<unsigned char>(text[idx + 1u])) &&
            isUtf8Continuation(static_cast<unsigned char>(text[idx + 2u]))) {
            return {3u, 1u};
        }
        return {1u, 1u};
    }
    if ((b0 & 0xF8u) == 0xF0u) {
        if (idx + 3u < text.size() &&
            isUtf8Continuation(static_cast<unsigned char>(text[idx + 1u])) &&
            isUtf8Continuation(static_cast<unsigned char>(text[idx + 2u])) &&
            isUtf8Continuation(static_cast<unsigned char>(text[idx + 3u]))) {
            return {4u, 2u};
        }
        return {1u, 1u};
    }
    return {1u, 1u};
}

} // namespace

std::size_t utf8ColumnToUtf16Column(
    std::string_view lineText,
    std::size_t utf8ByteColumn) noexcept {
    const std::size_t limit = std::min(utf8ByteColumn, lineText.size());
    std::size_t idx = 0;
    std::size_t utf16Col = 0;
    while (idx < limit) {
        const auto [byteLen, utf16Units] = decodeUtf8Step(lineText, idx);
        if (idx + byteLen > limit) {
            break;
        }
        idx += byteLen;
        utf16Col += utf16Units;
    }
    if (utf8ByteColumn > lineText.size()) {
        utf16Col += (utf8ByteColumn - lineText.size());
    }
    return utf16Col;
}

std::size_t utf16ColumnToUtf8Column(
    std::string_view lineText,
    std::size_t utf16Column) noexcept {
    std::size_t idx = 0;
    std::size_t utf16Col = 0;
    while (idx < lineText.size() && utf16Col < utf16Column) {
        const auto [byteLen, utf16Units] = decodeUtf8Step(lineText, idx);
        if (utf16Col + utf16Units > utf16Column) {
            break;
        }
        idx += byteLen;
        utf16Col += utf16Units;
    }
    return idx;
}

std::string_view lineContentAt(
    std::string_view documentText,
    std::size_t zeroBasedLine) noexcept {
    std::size_t curLine = 0;
    std::size_t i = 0;
    while (i < documentText.size() && curLine < zeroBasedLine) {
        if (documentText[i] == '\n') {
            ++curLine;
        }
        ++i;
    }
    if (curLine < zeroBasedLine) {
        return {};
    }
    std::size_t end = i;
    while (end < documentText.size() && documentText[end] != '\n') {
        ++end;
    }
    if (end > i && documentText[end - 1u] == '\r') {
        --end;
    }
    return documentText.substr(i, end - i);
}

LinePosition utf8ToUtf16Position(
    std::string_view documentText,
    LinePosition utf8Position) noexcept {
    const auto lineView = lineContentAt(documentText, utf8Position.line);
    return {
        utf8Position.line,
        utf8ColumnToUtf16Column(lineView, utf8Position.column),
    };
}

LinePosition utf16ToUtf8Position(
    std::string_view documentText,
    LinePosition utf16Position) noexcept {
    const auto lineView = lineContentAt(documentText, utf16Position.line);
    return {
        utf16Position.line,
        utf16ColumnToUtf8Column(lineView, utf16Position.column),
    };
}

} // namespace realscript::text
