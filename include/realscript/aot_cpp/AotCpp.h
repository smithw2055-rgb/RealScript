#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/mir/Mir.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace realscript::aot {

enum class AotEmitMode : std::uint8_t {
    FullMultiTier = 0,
    TrustedReleaseOnly = 1,
};

struct GenerationOptions {
    std::string programName = "RealScriptProgram";
    std::string cppNamespace = "realscript_generated";
    std::string querySymbol = "rs_module_query_v1";
    bool emitLineDirectives = true;
    bool trustedRelease = false;
    AotEmitMode emitMode = AotEmitMode::FullMultiTier;
    bool enableSharedReferenceGenerics = true;
    bool emitHotPatchThunks = false;
    bool collectOptimizationRemarks = false;
};

enum class OptimizationRemarkKind : std::uint8_t {
    Passed = 0,
    Missed = 1,
    Analysis = 2,
};

struct OptimizationRemark {
    OptimizationRemarkKind kind = OptimizationRemarkKind::Analysis;
    std::string pass;
    semantic::SymbolId functionSymbolId = 0;
    std::string functionName;
    std::string sourceFile;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string message;
};

struct FunctionLoweringSummary {
    semantic::SymbolId symbolId = 0;
    std::string moduleName;
    std::string functionName;
    bool typedPrimitiveAot = false;
    bool parallelBatchKernel = false;
    std::size_t heapAllocationSites = 0;
    bool isAsync = false;
    bool hasLoops = false;
    std::string boxedFallbackReason;
};

struct GeneratedProgram {
    std::string header;
    std::string source;
    std::string manifest;
    std::uint64_t contentHash = 0;
    std::size_t sharedGenericBodyCount = 0;
    std::size_t foldedGenericFunctionCount = 0;
    std::size_t uniqueCallSignatureCount = 0;
    std::size_t sharedPolyDispatchHelperCount = 0;
    std::vector<OptimizationRemark> optimizationRemarks;
};

class CppGenerator {
public:
    [[nodiscard]] GeneratedProgram generate(
        const std::vector<mir::Module>& modules,
        diagnostics::DiagnosticBag& diagnostics,
        GenerationOptions options = {}) const;
};

[[nodiscard]] std::vector<FunctionLoweringSummary> analyzeFunctionLowering(
    const std::vector<mir::Module>& modules);

[[nodiscard]] std::string sanitizeCppIdentifier(const std::string& value);
[[nodiscard]] std::string escapeCppString(const std::string& value);

} // namespace realscript::aot
