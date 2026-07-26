#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace realscript::optimization {

enum class Level : std::uint8_t {
    None = 0,
    Basic = 1,
    Aggressive = 2,
};

struct Options {
    Level level = Level::Basic;
    std::size_t maximumIterations = 4;
    bool preserveDebugInfo = true;
};

struct Statistics {
    std::uint64_t functionsVisited = 0;
    std::uint64_t iterations = 0;
    std::uint64_t constantsFolded = 0;
    std::uint64_t branchesFolded = 0;
    std::uint64_t instructionsRemoved = 0;
    std::uint64_t blocksRemoved = 0;
    std::uint64_t localLoadsFolded = 0;

    [[nodiscard]] bool changed() const noexcept;
};

struct Result {
    std::vector<mir::Module> modules;
    Statistics statistics;
};

class Optimizer {
public:
    [[nodiscard]] Result optimize(
        std::vector<mir::Module> modules,
        diagnostics::DiagnosticBag& diagnostics,
        Options options = {}) const;

    [[nodiscard]] mir::Module optimizeModule(
        mir::Module module,
        diagnostics::DiagnosticBag& diagnostics,
        Options options = {},
        Statistics* statistics = nullptr) const;
};

[[nodiscard]] const char* levelName(Level level) noexcept;
[[nodiscard]] std::string formatStatistics(const Statistics& statistics);

} // namespace realscript::optimization
