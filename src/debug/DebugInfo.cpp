#include "realscript/debug/DebugInfo.h"

#include <algorithm>

namespace realscript::debug {

text::LinePosition linePosition(
    const SourceFileInfo& file,
    std::size_t offset) noexcept {
    if (file.lineStarts.empty()) return {};
    const auto position = std::upper_bound(
        file.lineStarts.begin(), file.lineStarts.end(),
        static_cast<std::uint32_t>(offset));
    const auto line = position == file.lineStarts.begin()
        ? 0u
        : static_cast<std::size_t>(position - file.lineStarts.begin() - 1);
    const auto start = static_cast<std::size_t>(file.lineStarts[line]);
    return {line, offset >= start ? offset - start : 0};
}

std::size_t offsetAt(
    const SourceFileInfo& file,
    text::LinePosition position) noexcept {
    if (file.lineStarts.empty()) return position.column;
    const auto line = std::min(position.line, file.lineStarts.size() - 1);
    return static_cast<std::size_t>(file.lineStarts[line]) + position.column;
}

SourceRange makeSourceRange(
    const SourceFileInfo& file,
    text::TextSpan span) noexcept {
    SourceRange range;
    range.fileId = file.id;
    range.span = span;
    range.start = linePosition(file, span.start);
    range.end = linePosition(file, span.end());
    return range;
}

void finalizeFunctionDebugInfo(
    FunctionDebugInfo& info,
    const std::vector<SourceFileInfo>& files) noexcept {
    const SourceFileInfo* source = nullptr;
    for (const auto& file : files) {
        if (file.path == info.sourceName) {
            source = &file;
            break;
        }
    }
    if (!source && !files.empty()) source = &files.front();
    if (!source) return;
    info.sourceFileId = source->id;
    const auto resolve = [&](SourceRange& range) {
        range = makeSourceRange(*source, range.span);
    };
    resolve(info.declaration);
    resolve(info.body);
    for (auto& point : info.sequencePoints) {
        resolve(point.range);
        if (!source->lineDirectives.empty()) {
            const auto physLine = point.range.start.line;
            const auto it = std::upper_bound(
                source->lineDirectives.begin(),
                source->lineDirectives.end(),
                physLine,
                [](std::size_t targetLine, const text::LineDirectiveSpan& entry) {
                    return targetLine < entry.physicalStartLine;
                });
            if (it != source->lineDirectives.begin()) {
                const auto& active = *(it - 1);
                if (active.hidden) {
                    point.hidden = true;
                    point.mappedPath = active.mappedPath;
                } else if (!active.isDefault) {
                    const std::size_t delta = physLine >= active.physicalStartLine
                        ? (physLine - active.physicalStartLine)
                        : 0u;
                    const std::size_t endDelta = point.range.end.line >= physLine
                        ? (point.range.end.line - physLine)
                        : 0u;
                    point.range.start.line = active.mappedStartLine + delta;
                    point.range.end.line = point.range.start.line + endDelta;
                    point.mappedPath = active.mappedPath.empty() ? source->path : active.mappedPath;
                }
            }
        }
    }
    for (auto& local : info.locals) {
        resolve(local.declaration);
        resolve(local.scope);
    }
}

} // namespace realscript::debug
