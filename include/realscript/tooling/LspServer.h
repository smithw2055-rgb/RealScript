#pragma once

#include "realscript/tooling/Json.h"
#include "realscript/tooling/LanguageService.h"

#include <istream>
#include <ostream>
#include <string>
#include <unordered_set>

namespace realscript::tooling {

class LspServer {
public:
    enum class PositionEncoding : std::uint8_t {
        Utf16 = 0,
        Utf8 = 1,
    };

    int run(std::istream& input, std::ostream& output);
    [[nodiscard]] Json handle(const Json& message, std::ostream* notifications = nullptr);
    [[nodiscard]] LanguageService& service() noexcept { return service_; }
    [[nodiscard]] const LanguageService& service() const noexcept { return service_; }
    [[nodiscard]] PositionEncoding positionEncoding() const noexcept { return positionEncoding_; }

private:
    [[nodiscard]] Json response(const Json& request, Json result) const;
    [[nodiscard]] Json error(const Json& request, int code, std::string message) const;
    [[nodiscard]] std::string resolveDocumentText(const std::string& path) const;
    [[nodiscard]] text::LinePosition fromLspPosition(
        std::string_view documentText,
        const Json* value) const noexcept;
    [[nodiscard]] Json toLspPosition(
        std::string_view documentText,
        text::LinePosition utf8Pos) const;
    [[nodiscard]] Json toLspRange(
        std::string_view documentText,
        const debug::SourceRange& utf8Range) const;
    [[nodiscard]] Json toLspLocation(const Location& value) const;
    void publishDiagnostics(const std::string& path, std::ostream& output);
    void publishWorkspaceDiagnostics(std::ostream& output);

    LanguageService service_;
    std::unordered_set<std::string> publishedNonEmptyDiagnostics_;
    PositionEncoding positionEncoding_ = PositionEncoding::Utf16;
    bool shutdown_ = false;
};

} // namespace realscript::tooling
