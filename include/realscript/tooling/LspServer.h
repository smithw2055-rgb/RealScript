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
    int run(std::istream& input, std::ostream& output);
    [[nodiscard]] Json handle(const Json& message, std::ostream* notifications = nullptr);
    [[nodiscard]] LanguageService& service() noexcept { return service_; }
    [[nodiscard]] const LanguageService& service() const noexcept { return service_; }

private:
    [[nodiscard]] Json response(const Json& request, Json result) const;
    [[nodiscard]] Json error(const Json& request, int code, std::string message) const;
    void publishDiagnostics(const std::string& path, std::ostream& output);
    void publishWorkspaceDiagnostics(std::ostream& output);

    LanguageService service_;
    std::unordered_set<std::string> publishedNonEmptyDiagnostics_;
    bool shutdown_ = false;
};

} // namespace realscript::tooling
