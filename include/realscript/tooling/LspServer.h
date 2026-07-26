#pragma once

#include "realscript/tooling/Json.h"
#include "realscript/tooling/LanguageService.h"

#include <istream>
#include <ostream>

namespace realscript::tooling {

class LspServer {
public:
    int run(std::istream& input, std::ostream& output);
    [[nodiscard]] Json handle(const Json& message, std::ostream* notifications = nullptr);

private:
    [[nodiscard]] Json response(const Json& request, Json result) const;
    [[nodiscard]] Json error(const Json& request, int code, std::string message) const;
    void publishDiagnostics(const std::string& path, std::ostream& output);

    LanguageService service_;
    bool shutdown_ = false;
};

} // namespace realscript::tooling
