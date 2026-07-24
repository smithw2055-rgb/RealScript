#include "realscript/diagnostics/Diagnostic.h"

#include <sstream>

namespace realscript::diagnostics {

void DiagnosticBag::report(
    std::string code,
    std::string message,
    text::TextSpan span,
    DiagnosticSeverity severity) {
    diagnostics_.push_back({severity, std::move(code), std::move(message), span});
}

void DiagnosticBag::append(const DiagnosticBag& other) {
    diagnostics_.insert(diagnostics_.end(), other.items().begin(), other.items().end());
}

bool DiagnosticBag::hasErrors() const noexcept {
    for (const auto& diagnostic : diagnostics_) {
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            return true;
        }
    }
    return false;
}

std::string formatDiagnostic(const Diagnostic& diagnostic, const text::SourceText& source) {
    const auto position = source.linePosition(diagnostic.span.start);
    std::ostringstream out;
    out << source.name() << ':' << (position.line + 1) << ':' << (position.column + 1)
        << ": "
        << (diagnostic.severity == DiagnosticSeverity::Error ? "error" : "warning")
        << ' ' << diagnostic.code << ": " << diagnostic.message;
    return out.str();
}

} // namespace realscript::diagnostics
