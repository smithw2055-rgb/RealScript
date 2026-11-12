#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace realscript::tooling {

struct FormatOptions {
    std::size_t indentWidth = 4;
    std::size_t maxConsecutiveBlankLines = 1;
    bool insertFinalNewline = true;
};

struct FormatResult {
    std::string formatted;
    bool changed = false;
    bool unchangedOnError = false;
    std::string errorMessage;
};

[[nodiscard]] FormatResult formatSource(
    std::string_view sourceText,
    const FormatOptions& options = {});

} // namespace realscript::tooling
