#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"

#include <cstdint>
#include <string>
#include <vector>

namespace realscript::aot {

struct GenerationOptions {
    std::string programName = "RealScriptProgram";
    std::string cppNamespace = "realscript_generated";
    std::string querySymbol = "rs_module_query_v1";
    bool emitLineDirectives = true;
};

struct GeneratedProgram {
    std::string header;
    std::string source;
    std::string manifest;
    std::uint64_t contentHash = 0;
};

class CppGenerator {
public:
    [[nodiscard]] GeneratedProgram generate(
        const std::vector<mir::Module>& modules,
        diagnostics::DiagnosticBag& diagnostics,
        GenerationOptions options = {}) const;
};

[[nodiscard]] std::string sanitizeCppIdentifier(const std::string& value);
[[nodiscard]] std::string escapeCppString(const std::string& value);

} // namespace realscript::aot
