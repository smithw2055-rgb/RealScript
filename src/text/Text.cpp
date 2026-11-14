#include "realscript/text/Text.h"

#include <algorithm>
#include <limits>

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

MappedLineLocation SourceText::mappedLocation(std::size_t offset) const {
    const auto phys = linePosition(offset);
    if (lineDirectives_.empty()) {
        return {name_, phys, false};
    }
    const auto it = std::upper_bound(
        lineDirectives_.begin(),
        lineDirectives_.end(),
        phys.line,
        [](std::size_t targetLine, const LineDirectiveSpan& entry) {
            return targetLine < entry.physicalStartLine;
        });
    if (it == lineDirectives_.begin()) {
        return {name_, phys, false};
    }
    const auto& active = *(it - 1);
    if (active.isDefault) {
        return {name_, phys, false};
    }
    if (active.hidden) {
        return {active.mappedPath.empty() ? name_ : active.mappedPath, phys, true};
    }
    const std::size_t delta = phys.line >= active.physicalStartLine
        ? (phys.line - active.physicalStartLine)
        : 0u;
    return {
        active.mappedPath.empty() ? name_ : active.mappedPath,
        {active.mappedStartLine + delta, phys.column},
        false,
    };
}

bool SourceText::isHiddenOffset(std::size_t offset) const noexcept {
    if (lineDirectives_.empty()) return false;
    const auto phys = linePosition(offset);
    const auto it = std::upper_bound(
        lineDirectives_.begin(),
        lineDirectives_.end(),
        phys.line,
        [](std::size_t targetLine, const LineDirectiveSpan& entry) {
            return targetLine < entry.physicalStartLine;
        });
    if (it == lineDirectives_.begin()) return false;
    return (it - 1)->hidden;
}

void SourceText::buildLineMap() {
    lineStarts_.clear();
    lineDirectives_.clear();
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

    std::string activePath = name_;
    bool inBlockComment = false;
    bool inVerbatimString = false;

    const auto isHorizWs = [](char ch) noexcept {
        return ch == ' ' || ch == '\t';
    };
    const auto isDirectiveTerminator = [](char ch) noexcept {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };

    for (std::size_t lineIdx = 0; lineIdx < lineStarts_.size(); ++lineIdx) {
        std::size_t pos = lineStarts_[lineIdx];
        const std::size_t end = (lineIdx + 1 < lineStarts_.size())
            ? lineStarts_[lineIdx + 1]
            : content_.size();
        if (lineIdx == 0 && pos + 3 <= end &&
            static_cast<unsigned char>(content_[pos]) == 0xEFu &&
            static_cast<unsigned char>(content_[pos + 1]) == 0xBBu &&
            static_cast<unsigned char>(content_[pos + 2]) == 0xBFu) {
            pos += 3;
        }

        if (!inBlockComment && !inVerbatimString) {
            std::size_t dirPos = pos;
            while (dirPos < end && isHorizWs(content_[dirPos])) {
                ++dirPos;
            }
            if (dirPos < end && content_[dirPos] == '#') {
                ++dirPos;
                while (dirPos < end && isHorizWs(content_[dirPos])) {
                    ++dirPos;
                }
                if (end - dirPos >= 4 &&
                    std::string_view(content_).substr(dirPos, 4) == "line") {
                    dirPos += 4;
                    if (dirPos == end || isDirectiveTerminator(content_[dirPos])) {
                        while (dirPos < end && isHorizWs(content_[dirPos])) {
                            ++dirPos;
                        }
                        const std::size_t nextPhysLine = lineIdx + 1;
                        if (end - dirPos >= 6 &&
                            std::string_view(content_).substr(dirPos, 6) == "hidden" &&
                            (dirPos + 6 == end || isDirectiveTerminator(content_[dirPos + 6]))) {
                            lineDirectives_.push_back({nextPhysLine, 0, activePath, true, false});
                            continue;
                        }
                        if (end - dirPos >= 7 &&
                            std::string_view(content_).substr(dirPos, 7) == "default" &&
                            (dirPos + 7 == end || isDirectiveTerminator(content_[dirPos + 7]))) {
                            activePath = name_;
                            lineDirectives_.push_back({nextPhysLine, nextPhysLine, name_, false, true});
                            continue;
                        }
                        if (dirPos < end && content_[dirPos] >= '0' && content_[dirPos] <= '9') {
                            constexpr std::size_t kMaxLine =
                                (std::numeric_limits<std::size_t>::max)();
                            bool overflow = false;
                            std::size_t mappedOneBased = 0;
                            while (dirPos < end && content_[dirPos] >= '0' && content_[dirPos] <= '9') {
                                const auto digit =
                                    static_cast<std::size_t>(content_[dirPos] - '0');
                                if (mappedOneBased > (kMaxLine - digit) / 10u) {
                                    overflow = true;
                                    break;
                                }
                                mappedOneBased = mappedOneBased * 10u + digit;
                                ++dirPos;
                            }
                            if (!overflow &&
                                (dirPos == end || isDirectiveTerminator(content_[dirPos]))) {
                                while (dirPos < end && isHorizWs(content_[dirPos])) {
                                    ++dirPos;
                                }
                                if (dirPos < end && content_[dirPos] == '"') {
                                    ++dirPos;
                                    std::string parsedPath;
                                    while (dirPos < end && content_[dirPos] != '"' &&
                                           content_[dirPos] != '\r' && content_[dirPos] != '\n') {
                                        parsedPath.push_back(content_[dirPos]);
                                        ++dirPos;
                                    }
                                    if (!parsedPath.empty()) {
                                        activePath = std::move(parsedPath);
                                    }
                                }
                                const std::size_t zeroBased =
                                    mappedOneBased > 0 ? (mappedOneBased - 1) : 0u;
                                lineDirectives_.push_back(
                                    {nextPhysLine, zeroBased, activePath, false, false});
                            }
                        }
                        continue;
                    }
                }
            }
        }

        while (pos < end) {
            if (inBlockComment) {
                if (pos + 1 < end && content_[pos] == '*' && content_[pos + 1] == '/') {
                    inBlockComment = false;
                    pos += 2;
                } else {
                    ++pos;
                }
                continue;
            }
            if (inVerbatimString) {
                if (content_[pos] == '"') {
                    if (pos + 1 < end && content_[pos + 1] == '"') {
                        pos += 2;
                    } else {
                        inVerbatimString = false;
                        ++pos;
                    }
                } else {
                    ++pos;
                }
                continue;
            }

            if (pos + 1 < end && content_[pos] == '/' && content_[pos + 1] == '/') {
                break;
            }
            if (pos + 1 < end && content_[pos] == '/' && content_[pos + 1] == '*') {
                inBlockComment = true;
                pos += 2;
                continue;
            }
            if (pos + 1 < end && content_[pos] == '@' && content_[pos + 1] == '"') {
                inVerbatimString = true;
                pos += 2;
                continue;
            }
            if (pos + 2 < end &&
                ((content_[pos] == '$' && content_[pos + 1] == '@' && content_[pos + 2] == '"') ||
                 (content_[pos] == '@' && content_[pos + 1] == '$' && content_[pos + 2] == '"'))) {
                inVerbatimString = true;
                pos += 3;
                continue;
            }
            if (content_[pos] == '"') {
                ++pos;
                while (pos < end) {
                    if (content_[pos] == '\\' && pos + 1 < end) {
                        pos += 2;
                    } else if (content_[pos] == '"') {
                        ++pos;
                        break;
                    } else {
                        ++pos;
                    }
                }
                continue;
            }
            if (content_[pos] == '\'') {
                ++pos;
                while (pos < end) {
                    if (content_[pos] == '\\' && pos + 1 < end) {
                        pos += 2;
                    } else if (content_[pos] == '\'') {
                        ++pos;
                        break;
                    } else {
                        ++pos;
                    }
                }
                continue;
            }
            ++pos;
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
