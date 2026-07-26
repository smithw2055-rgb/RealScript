#pragma once

#include "realscript/semantic/Semantic.h"
#include "realscript/text/Text.h"

#include <cstdint>
#include <string>
#include <vector>

namespace realscript::debug {

using SourceFileId = std::uint32_t;

struct SourceFileInfo {
    SourceFileId id = 0;
    std::string path;
    std::uint64_t contentHash = 0;
    std::vector<std::uint32_t> lineStarts;
};

struct SourceRange {
    SourceFileId fileId = 0;
    text::TextSpan span;
    text::LinePosition start;
    text::LinePosition end;

    [[nodiscard]] bool valid() const noexcept {
        return !span.empty() || start.line != 0 || start.column != 0 ||
            end.line != 0 || end.column != 0;
    }
};

struct SequencePoint {
    std::uint32_t blockId = 0;
    std::uint32_t instructionIndex = 0;
    bool terminator = false;
    SourceRange range;
};

struct LocalVariableInfo {
    std::string name;
    std::uint32_t slot = 0;
    semantic::PrimitiveType type = semantic::PrimitiveType::Error;
    semantic::SymbolId typeId = 0;
    bool parameter = false;
    SourceRange declaration;
    SourceRange scope;
};

struct FunctionDebugInfo {
    std::string sourceName;
    SourceFileId sourceFileId = 0;
    SourceRange declaration;
    SourceRange body;
    std::vector<SequencePoint> sequencePoints;
    std::vector<LocalVariableInfo> locals;
};

[[nodiscard]] SourceRange makeSourceRange(
    const SourceFileInfo& file,
    text::TextSpan span) noexcept;
[[nodiscard]] text::LinePosition linePosition(
    const SourceFileInfo& file,
    std::size_t offset) noexcept;
[[nodiscard]] std::size_t offsetAt(
    const SourceFileInfo& file,
    text::LinePosition position) noexcept;
void finalizeFunctionDebugInfo(
    FunctionDebugInfo& info,
    const std::vector<SourceFileInfo>& files) noexcept;

} // namespace realscript::debug
