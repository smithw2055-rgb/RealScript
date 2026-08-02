#include "realscript/compiler/LanguageExpansion.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realscript::compiler {
namespace {

#include "LanguageExpansionCore.inl"
#include "LanguageExpansionDeclarations.inl"
#include "LanguageExpansionStatements.inl"

} // namespace

bool LanguageExpansionResult::succeeded() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const LanguageExpansionDiagnostic& diagnostic) {
            return diagnostic.severity == LanguageExpansionSeverity::Error;
        });
}

LanguageExpansionResult expandLanguageSource(
    const std::string& path,
    const std::string& content,
    LanguageExpansionOptions options) {
    Context context;
    context.path = path;
    context.options = options;
    auto tokens = lex(content);

    rewriteValueAliases(tokens, context);
    extractAttributes(tokens, context);
    extractDelegates(tokens, context);
    extractInterfaces(tokens, context);
    collectGenericDeclarations(tokens, context);
    instantiateGenerics(tokens, context);
    applyInterfaces(tokens, context);
    lowerEvents(tokens, context);
    lowerSequences(tokens, context);
    lowerReferenceParameters(tokens, context);
    lowerFunctionBodies(tokens, context);
    appendGeneratedSupport(tokens, context);

    context.result.content = emit(tokens);
    if (context.result.content.empty()) context.result.content = content;
    return std::move(context.result);
}

const char* languageExpansionSeverityName(
    LanguageExpansionSeverity severity) noexcept {
    switch (severity) {
    case LanguageExpansionSeverity::Warning: return "warning";
    case LanguageExpansionSeverity::Error: return "error";
    }
    return "unknown";
}

} // namespace realscript::compiler
