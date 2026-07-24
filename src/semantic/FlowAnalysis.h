#pragma once

#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/semantic/Semantic.h"

namespace realscript::semantic::detail {

[[nodiscard]] bool canReachFunctionEnd(
    const BoundFunction& function,
    diagnostics::DiagnosticBag& diagnostics);

} // namespace realscript::semantic::detail
