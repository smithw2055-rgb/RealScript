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
    std::string moduleName;
    std::vector<std::string> imports;
    std::vector<Token> tokens;
    LanguageExpansionResult preliminary;
};

std::string qualifiedName(
    const std::vector<Token>& tokens,
    std::size_t begin,
    std::size_t end) {
    std::string result;
    bool expectIdentifier = true;
    for (std::size_t index = begin; index < end; ++index) {
        if (tokens[index].kind == TokenKind::Identifier && expectIdentifier) {
            result += tokens[index].text;
            expectIdentifier = false;
            continue;
        }
        if (symbol(tokens[index], ".") && !expectIdentifier) {
            result.push_back('.');
            expectIdentifier = true;
            continue;
        }
        break;
    }
    if (result.empty() || expectIdentifier) return {};
    return result;
}

std::string sourceModuleName(
    const std::vector<Token>& tokens,
    const std::string& path) {
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (!word(tokens[index], "module")) continue;
        std::size_t end = index + 1;
        while (end < tokens.size() && !symbol(tokens[end], ";")) ++end;
        const auto name = qualifiedName(tokens, index + 1, end);
        if (!name.empty()) return name;
        break;
    }
    return "$invalid$" + path;
}

std::vector<std::string> sourceImports(const std::vector<Token>& tokens) {
    std::vector<std::string> result;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (!word(tokens[index], "import")) continue;
        std::size_t end = index + 1;
        while (end < tokens.size() &&
               !symbol(tokens[end], ";") &&
               !word(tokens[end], "as")) {
            ++end;
        }
        const auto name = qualifiedName(tokens, index + 1, end);
        if (!name.empty() &&
            std::find(result.begin(), result.end(), name) == result.end()) {
            result.push_back(name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

template <typename Map>
void mergeDeclarations(Map& target, const Map& source) {
    for (const auto& entry : source) target.emplace(entry.first, entry.second);
}

std::size_t genericDeclarationStart(
    const std::vector<Token>& tokens,
    std::size_t declaration) {
    auto start = declaration;
    while (start > 0 && symbol(tokens[start - 1], "]")) {
        auto cursor = start - 1;
        int depth = 1;
        while (cursor > 0 && depth != 0) {
            --cursor;
            if (symbol(tokens[cursor], "]")) ++depth;
            else if (symbol(tokens[cursor], "[")) --depth;
        }
        if (depth != 0) break;
        start = cursor;
    }
    return start;
}

void collectGenericDeclarationsWithInterfaces(
    std::vector<Token>& tokens,
    Context& context) {
    if (!context.options.generics) return;
    std::vector<std::pair<std::size_t, std::size_t>> remove;
    int braceDepth = 0;
    for (std::size_t index = 0; index + 3 < tokens.size();) {
        if (symbol(tokens[index], "{")) {
            ++braceDepth;
            ++index;
            continue;
        }
        if (symbol(tokens[index], "}")) {
            --braceDepth;
            ++index;
            continue;
        }

        if (braceDepth == 0 &&
            (word(tokens[index], "class") || word(tokens[index], "struct")) &&
            tokens[index + 1].kind == TokenKind::Identifier &&
            symbol(tokens[index + 2], "<")) {
            const auto angleClose = matching(tokens, index + 2, "<", ">");
            if (angleClose >= tokens.size()) {
                ++index;
                continue;
            }
            std::size_t bodyOpen = angleClose + 1;
            while (bodyOpen < tokens.size() &&
                   !symbol(tokens[bodyOpen], "{") &&
                   !symbol(tokens[bodyOpen], ";")) {
                ++bodyOpen;
            }
            if (bodyOpen >= tokens.size() || !symbol(tokens[bodyOpen], "{")) {
                ++index;
                continue;
            }
            const auto bodyClose = matching(tokens, bodyOpen, "{", "}");
            if (bodyClose >= tokens.size()) break;

            const auto declarationStart =
                genericDeclarationStart(tokens, index);
            GenericDecl declaration;
            declaration.kind = GenericDecl::Kind::Type;
            declaration.name = tokens[index + 1].text;
            declaration.parameters = parseTypeParameterNames(
                tokens, index + 2, angleClose);
            declaration.tokens.assign(
                tokens.begin() +
                    static_cast<std::ptrdiff_t>(declarationStart),
                tokens.begin() +
                    static_cast<std::ptrdiff_t>(bodyClose + 1));
            context.generics[declaration.name] = std::move(declaration);
            remove.push_back({declarationStart, bodyClose + 1});
            index = bodyClose + 1;
            continue;
        }

        if (braceDepth == 0 && tokens[index].kind == TokenKind::Identifier &&
            index + 1 < tokens.size() && symbol(tokens[index + 1], "<")) {
            const auto angleClose = matching(tokens, index + 1, "<", ">");
            if (angleClose < tokens.size() && angleClose + 1 < tokens.size() &&
                symbol(tokens[angleClose + 1], "(")) {
                const auto parameterClose = matching(
                    tokens, angleClose + 1, "(", ")");
                if (parameterClose < tokens.size() &&
                    parameterClose + 1 < tokens.size() &&
                    symbol(tokens[parameterClose + 1], "{")) {
                    std::size_t declarationStart = index;
                    while (declarationStart > 0 &&
                           !symbol(tokens[declarationStart - 1], ";") &&
                           !symbol(tokens[declarationStart - 1], "}")) {
                        --declarationStart;
                    }
                    const auto bodyClose = matching(
                        tokens, parameterClose + 1, "{", "}");
                    if (bodyClose >= tokens.size()) break;
                    GenericDecl declaration;
                    declaration.kind = GenericDecl::Kind::Function;
                    declaration.name = tokens[index].text;
                    declaration.parameters = parseTypeParameterNames(
                        tokens, index + 1, angleClose);
                    declaration.tokens.assign(
                        tokens.begin() + static_cast<std::ptrdiff_t>(declarationStart),
                        tokens.begin() + static_cast<std::ptrdiff_t>(bodyClose + 1));
                    context.generics[declaration.name] = std::move(declaration);
                    remove.push_back({declarationStart, bodyClose + 1});
                    index = bodyClose + 1;
                    continue;
                }
            }
        }
        ++index;
    }

    if (!remove.empty()) {
        std::sort(remove.begin(), remove.end());
        tokens = removeRanges(tokens, remove);
        context.result.changed = true;
    }
}

void collectExpansionDeclarations(
    PreparedExpansionSource& source,
    Context& context) {
    context.path = source.path;
    context.result = {};
    if (source.tokens.empty()) source.tokens = lex(source.originalContent);

    rewriteValueAliases(source.tokens, context);
    extractAttributes(source.tokens, context);
    extractDelegates(source.tokens, context);
    extractInterfaces(source.tokens, context);
    collectGenericDeclarationsWithInterfaces(source.tokens, context);

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

bool classHasMethod(
    const std::vector<Token>& tokens,
    std::size_t open,
    std::size_t close,
    const std::string& methodName) {
    int depth = 0;
    for (std::size_t index = open + 1; index + 1 < close; ++index) {
        if (symbol(tokens[index], "{")) {
            ++depth;
            continue;
        }
        if (symbol(tokens[index], "}")) {
            --depth;
            continue;
        }
        if (depth == 0 && tokens[index].kind == TokenKind::Identifier &&
            tokens[index].text == methodName && symbol(tokens[index + 1], "(")) {
            return true;
        }
    }
    return false;
}

std::vector<Token> collectionElementType(
    const std::vector<Token>& tokens,
    std::size_t open,
    std::size_t close,
    const std::string& fieldName) {
    for (std::size_t index = open + 1; index < close; ++index) {
        if (tokens[index].kind != TokenKind::Identifier ||
            tokens[index].text != fieldName || index < 2 ||
            !symbol(tokens[index - 1], "]")) {
            continue;
        }
        std::size_t arrayOpen = index - 1;
        int depth = 1;
        while (arrayOpen > open + 1 && depth != 0) {
            --arrayOpen;
            if (symbol(tokens[arrayOpen], "]")) ++depth;
            else if (symbol(tokens[arrayOpen], "[")) --depth;
        }
        if (depth != 0 || arrayOpen <= open + 1) return {};
        return std::vector<Token>(
            tokens.begin() + static_cast<std::ptrdiff_t>(open + 1),
            tokens.begin() + static_cast<std::ptrdiff_t>(arrayOpen));
    }
    return {};
}

void addCollectionEnumerationHelpers(std::vector<Token>& tokens) {
    for (std::size_t index = 0; index + 2 < tokens.size();) {
        if (!word(tokens[index], "class") ||
            tokens[index + 1].kind != TokenKind::Identifier) {
            ++index;
            continue;
        }
        const auto& name = tokens[index + 1].text;
        const bool queue = name.rfind("Queue__", 0) == 0;
        const bool stack = name.rfind("Stack__", 0) == 0;
        const bool set = name.rfind("HashSet__", 0) == 0;
        if (!queue && !stack && !set) {
            ++index;
            continue;
        }
        std::size_t open = index + 2;
        while (open < tokens.size() && !symbol(tokens[open], "{")) ++open;
        if (open >= tokens.size()) break;
        const auto close = matching(tokens, open, "{", "}");
        if (close >= tokens.size()) break;
        if (classHasMethod(tokens, open, close, "Get")) {
            index = close + 1;
            continue;
        }

        const auto fieldName = set ? std::string{"values"} : std::string{"items"};
        const auto typeTokens = collectionElementType(
            tokens, open, close, fieldName);
        if (typeTokens.empty()) {
            index = close + 1;
            continue;
        }
        const auto type = tokenText(typeTokens);
        std::ostringstream method;
        method << type << " Get(int index){return " << fieldName << '[';
        if (queue) method << "(head+index)%" << fieldName << ".length";
        else method << "index";
        method << "];}";
        auto generated = lex(method.str());
        if (!generated.empty() && generated.back().kind == TokenKind::End) {
            generated.pop_back();
        }
        tokens.insert(
            tokens.begin() + static_cast<std::ptrdiff_t>(close),
            generated.begin(), generated.end());
        index = close + generated.size() + 1;
    }
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
    addCollectionEnumerationHelpers(source.tokens);

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
        auto tokens = lex(source.content);
        prepared.push_back(PreparedExpansionSource{
            source.path,
            source.content,
            sourceModuleName(tokens, source.path),
            sourceImports(tokens),
            std::move(tokens),
            {}});
    }

    std::vector<std::size_t> order(sources.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
        [&](std::size_t left, std::size_t right) {
            if (prepared[left].moduleName != prepared[right].moduleName) {
                return prepared[left].moduleName < prepared[right].moduleName;
            }
            if (prepared[left].path != prepared[right].path) {
                return prepared[left].path < prepared[right].path;
            }
            return left < right;
        });

    std::map<std::string, Context> moduleContexts;
    std::map<std::string, std::set<std::string>> moduleImports;
    for (const auto index : order) {
        auto& context = moduleContexts[prepared[index].moduleName];
        context.options = options;
        moduleImports[prepared[index].moduleName].insert(
            prepared[index].imports.begin(), prepared[index].imports.end());
        collectExpansionDeclarations(prepared[index], context);
    }

    auto effectiveContexts = moduleContexts;
    for (auto& [moduleName, context] : effectiveContexts) {
        const auto imports = moduleImports.find(moduleName);
        if (imports == moduleImports.end()) continue;
        for (const auto& importedName : imports->second) {
            const auto imported = moduleContexts.find(importedName);
            if (imported == moduleContexts.end()) continue;
            mergeDeclarations(context.interfaces, imported->second.interfaces);
            mergeDeclarations(context.delegates, imported->second.delegates);
            mergeDeclarations(context.generics, imported->second.generics);
            mergeDeclarations(
                context.referenceFunctions,
                imported->second.referenceFunctions);
        }
    }

    std::map<std::string, std::set<std::string>> emittedGenericNames;
    std::map<std::string, std::set<std::string>> emittedReferenceTypes;
    for (const auto index : order) {
        auto& context = effectiveContexts[prepared[index].moduleName];
        results[index] = finishExpansion(
            prepared[index],
            context,
            emittedGenericNames[prepared[index].moduleName],
            emittedReferenceTypes[prepared[index].moduleName]);
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
