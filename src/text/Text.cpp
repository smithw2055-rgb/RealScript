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

} // namespace realscript::text
