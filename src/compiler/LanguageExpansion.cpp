#include "realscript/compiler/LanguageExpansion.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <numeric>
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

struct PreparedExpansionSource {
    std::string path;
    std::string originalContent;
    std::vector<Token> tokens;
    LanguageExpansionResult preliminary;
};

void collectExpansionDeclarations(
    PreparedExpansionSource& source,
    Context& context) {
    context.path = source.path;
    context.result = {};
    source.tokens = lex(source.originalContent);

    rewriteValueAliases(source.tokens, context);
    extractAttributes(source.tokens, context);
    extractDelegates(source.tokens, context);
    extractInterfaces(source.tokens, context);
    collectGenericDeclarations(source.tokens, context);

    source.preliminary = std::move(context.result);
}

bool eraseGeneratedDeclaration(
    std::vector<Token>& tokens,
    const std::string& generatedName) {
    int braceDepth = 0;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (symbol(tokens[index], "{")) {
            ++braceDepth;
            continue;
        }
        if (symbol(tokens[index], "}")) {
            --braceDepth;
            continue;
        }
        if (braceDepth != 0) continue;

        if ((word(tokens[index], "class") || word(tokens[index], "struct")) &&
            tokens[index + 1].kind == TokenKind::Identifier &&
            tokens[index + 1].text == generatedName) {
            std::size_t open = index + 2;
            while (open < tokens.size() && !symbol(tokens[open], "{")) ++open;
            if (open >= tokens.size()) return false;
            const auto close = matching(tokens, open, "{", "}");
            if (close >= tokens.size()) return false;
            tokens.erase(
                tokens.begin() + static_cast<std::ptrdiff_t>(index),
                tokens.begin() + static_cast<std::ptrdiff_t>(close + 1));
            return true;
        }

        if (tokens[index].kind == TokenKind::Identifier &&
            tokens[index].text == generatedName &&
            symbol(tokens[index + 1], "(")) {
            const auto closeParen = matching(tokens, index + 1, "(", ")");
            if (closeParen >= tokens.size() || closeParen + 1 >= tokens.size() ||
                !symbol(tokens[closeParen + 1], "{")) {
                continue;
            }
            const auto closeBody = matching(tokens, closeParen + 1, "{", "}");
            if (closeBody >= tokens.size()) return false;
            std::size_t begin = index;
            while (begin > 0 &&
                   !symbol(tokens[begin - 1], ";") &&
                   !symbol(tokens[begin - 1], "}")) {
                --begin;
            }
            tokens.erase(
                tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                tokens.begin() + static_cast<std::ptrdiff_t>(closeBody + 1));
            return true;
        }
    }
    return false;
}

LanguageExpansionResult finishExpansion(
    PreparedExpansionSource& source,
    Context& context,
    std::set<std::string>& emittedGenericNames,
    std::set<std::string>& emittedReferenceTypes) {
    context.path = source.path;
    context.result = std::move(source.preliminary);
    context.generatedRefTypes.clear();

    instantiateGenerics(source.tokens, context);

    std::set<std::string> sourceGenericNames;
    for (const auto& instantiation : context.result.genericInstantiations) {
        sourceGenericNames.insert(instantiation.generatedName);
    }
    for (const auto& generatedName : sourceGenericNames) {
        if (!emittedGenericNames.insert(generatedName).second) {
            static_cast<void>(eraseGeneratedDeclaration(
                source.tokens, generatedName));
        }
    }

    applyInterfaces(source.tokens, context);
    lowerEvents(source.tokens, context);
    lowerSequences(source.tokens, context);
    lowerReferenceParameters(source.tokens, context);
    lowerFunctionBodies(source.tokens, context);

    std::set<std::string> newReferenceTypes;
    for (const auto& encoded : context.generatedRefTypes) {
        if (emittedReferenceTypes.find(encoded) == emittedReferenceTypes.end()) {
            newReferenceTypes.insert(encoded);
        }
    }
    context.generatedRefTypes = newReferenceTypes;
    appendGeneratedSupport(source.tokens, context);
    emittedReferenceTypes.insert(
        newReferenceTypes.begin(), newReferenceTypes.end());
    context.generatedRefTypes.clear();

    context.result.content = emit(source.tokens);
    if (context.result.content.empty()) {
        context.result.content = source.originalContent;
    }
    return std::move(context.result);
}

} // namespace

bool LanguageExpansionResult::succeeded() const noexcept {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const LanguageExpansionDiagnostic& diagnostic) {
            return diagnostic.severity == LanguageExpansionSeverity::Error;
        });
}

std::vector<LanguageExpansionResult> expandLanguageSources(
    const std::vector<LanguageExpansionSource>& sources,
    LanguageExpansionOptions options) {
    std::vector<LanguageExpansionResult> results(sources.size());
    if (sources.empty()) return results;

    std::vector<PreparedExpansionSource> prepared;
    prepared.reserve(sources.size());
    for (const auto& source : sources) {
        prepared.push_back(PreparedExpansionSource{
            source.path, source.content, {}, {}});
    }

    std::vector<std::size_t> order(sources.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
        [&](std::size_t left, std::size_t right) {
            if (prepared[left].path != prepared[right].path) {
                return prepared[left].path < prepared[right].path;
            }
            return left < right;
        });

    Context context;
    context.options = options;

    // Pass one removes and records declarations that must be visible to sibling
    // files. This makes interfaces, delegates, and generic declarations module-
    // compilation features instead of accidental single-file features.
    for (const auto index : order) {
        collectExpansionDeclarations(prepared[index], context);
    }

    // Pass two performs executable lowering. Generated specializations and ref
    // wrappers are emitted once in stable path order, preventing duplicate type
    // declarations when several files use the same language feature.
    std::set<std::string> emittedGenericNames;
    std::set<std::string> emittedReferenceTypes;
    for (const auto index : order) {
        results[index] = finishExpansion(
            prepared[index],
            context,
            emittedGenericNames,
            emittedReferenceTypes);
    }
    return results;
}

LanguageExpansionResult expandLanguageSource(
    const std::string& path,
    const std::string& content,
    LanguageExpansionOptions options) {
    auto results = expandLanguageSources(
        {LanguageExpansionSource{path, content}}, options);
    return results.empty() ? LanguageExpansionResult{} : std::move(results.front());
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
