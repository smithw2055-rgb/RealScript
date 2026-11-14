#pragma once

#include "realscript/text/Text.h"

#include <string>
#include <vector>

namespace realscript::diagnostics {

enum class DiagnosticSeverity {
    Warning,
    Error,
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string code;
    std::string message;
    text::TextSpan span;
    std::string sourceName;
};

class DiagnosticBag {
public:
    void report(
        std::string code,
        std::string message,
        text::TextSpan span,
        DiagnosticSeverity severity = DiagnosticSeverity::Error,
        std::string sourceName = {});

    void append(const DiagnosticBag& other);
    void truncate(std::size_t count) noexcept {
        if (count < diagnostics_.size()) {
            diagnostics_.resize(count);
        }
    }
    [[nodiscard]] bool hasErrors() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return diagnostics_.empty(); }
    [[nodiscard]] const std::vector<Diagnostic>& items() const noexcept { return diagnostics_; }

private:
    std::vector<Diagnostic> diagnostics_;
};

[[nodiscard]] std::string formatDiagnostic(
    const Diagnostic& diagnostic,
    const text::SourceText& source);

} // namespace realscript::diagnostics
