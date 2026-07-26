#include "realscript/tooling/LanguageService.h"

#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_set>

namespace realscript::tooling {
namespace {

std::string typeDetail(semantic::PrimitiveType type, const std::string& exact = {}) {
    if (!exact.empty()) return exact;
    return semantic::primitiveTypeName(type);
}

bool identifier(const std::string& value) {
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) {
        return false;
    }
    for (const auto character : value) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_')) return false;
    }
    return true;
}

std::string functionDetail(const semantic::FunctionSymbol& function) {
    std::string output = typeDetail(function.returnType, function.returnTypeName) + " " + function.name + "(";
    std::size_t begin = function.method && !function.staticMethod ? 1 : 0;
    for (std::size_t index = begin; index < function.parameters.size(); ++index) {
        if (index != begin) output += ", ";
        output += typeDetail(function.parameters[index].type, function.parameters[index].typeName);
        output += " " + function.parameters[index].name;
    }
    output += ")";
    return output;
}

} // namespace

void LanguageService::open(std::string path, std::string content, std::int64_t version) {
    documents_[std::move(path)] = {std::move(content), version};
    dirty_ = true;
}

void LanguageService::update(std::string path, std::string content, std::int64_t version) {
    open(std::move(path), std::move(content), version);
}

void LanguageService::close(const std::string& path) {
    documents_.erase(path);
    dirty_ = true;
}

bool LanguageService::contains(const std::string& path) const noexcept {
    return documents_.find(path) != documents_.end();
}

std::vector<std::string> LanguageService::documentPaths() const {
    std::vector<std::string> paths;
    paths.reserve(documents_.size());
    for (const auto& [path, value] : documents_) {
        (void)value;
        paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

const compiler::BuildResult& LanguageService::build() {
    if (dirty_) rebuild();
    return result_;
}

void LanguageService::rebuild() {
    compiler::Compilation compilation;
    std::vector<std::string> paths;
    paths.reserve(documents_.size());
    for (const auto& [path, documentValue] : documents_) {
        (void)documentValue;
        paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        compilation.addSource({path, documents_.at(path).content});
    }
    result_ = compilation.build(hasSnapshot_ ? &snapshot_ : nullptr);
    snapshot_ = result_.snapshot;
    hasSnapshot_ = true;
    dirty_ = false;
}

const LanguageService::Document* LanguageService::document(const std::string& path) const noexcept {
    const auto found = documents_.find(path);
    return found == documents_.end() ? nullptr : &found->second;
}

debug::SourceRange LanguageService::rangeFor(
    const std::string& path,
    text::TextSpan span) const {
    const auto* value = document(path);
    if (!value) return {};
    text::SourceText source(value->content, path);
    debug::SourceFileInfo file;
    file.id = 0;
    file.path = path;
    file.lineStarts.push_back(0);
    for (std::size_t offset = 0; offset < value->content.size(); ++offset) {
        if (value->content[offset] == '\n') file.lineStarts.push_back(static_cast<std::uint32_t>(offset + 1));
    }
    return debug::makeSourceRange(file, span);
}

std::optional<syntax::SyntaxToken> LanguageService::tokenAt(
    const std::string& path,
    text::LinePosition position) const {
    const auto* value = document(path);
    if (!value) return std::nullopt;
    text::SourceText source(value->content, path);
    diagnostics::DiagnosticBag diagnostics;
    syntax::Lexer lexer(source, diagnostics);
    const auto offset = [&] {
        std::size_t line = 0;
        std::size_t start = 0;
        while (line < position.line && start < value->content.size()) {
            const auto next = value->content.find('\n', start);
            if (next == std::string::npos) return value->content.size();
            start = next + 1;
            ++line;
        }
        return std::min(start + position.column, value->content.size());
    }();
    for (const auto& token : lexer.lexAll()) {
        if (token.kind == syntax::SyntaxKind::EndOfFileToken) continue;
        if (offset >= token.span.start && offset <= token.span.end()) return token;
    }
    return std::nullopt;
}

std::vector<DocumentSymbol> LanguageService::allDefinitions() const {
    std::vector<DocumentSymbol> symbols;
    std::unordered_set<semantic::SymbolId> seen;
    auto append = [&](semantic::SymbolId id, semantic::SymbolKind kind,
                      std::string name, std::string detail,
                      const std::string& path, text::TextSpan span) {
        if (id == 0 || path.empty() || !seen.insert(id).second) return;
        symbols.push_back({id, kind, std::move(name), std::move(detail), {path, rangeFor(path, span)}});
    };

    for (const auto& occurrence : result_.symbols) {
        if (occurrence.definition) {
            append(occurrence.id, occurrence.kind, occurrence.name, occurrence.detail,
                occurrence.sourceName, occurrence.span);
        }
    }
    for (const auto& module : result_.modules) {
        for (const auto& type : module.types) {
            append(type.id, semantic::SymbolKind::Type, type.name,
                semantic::canonicalTypeName(type), type.sourceName, type.declarationSpan);
            for (const auto& field : type.fields) {
                append(field.id, semantic::SymbolKind::Field, field.name,
                    typeDetail(field.type, field.typeName), field.sourceName, field.declarationSpan);
            }
            for (const auto& member : type.enumMembers) {
                append(member.id, semantic::SymbolKind::EnumMember, member.name,
                    std::to_string(member.value), member.sourceName, member.declarationSpan);
            }
            for (const auto& property : type.properties) {
                append(property.id, semantic::SymbolKind::Property, property.name,
                    typeDetail(property.type, property.typeName), property.sourceName, property.declarationSpan);
            }
            for (const auto& method : type.methods) {
                append(method.id, semantic::SymbolKind::Function, method.name,
                    functionDetail(method), method.sourceName, method.declarationSpan);
            }
            for (const auto& constructor : type.constructors) {
                append(constructor.id, semantic::SymbolKind::Function, type.name,
                    functionDetail(constructor), constructor.sourceName, constructor.declarationSpan);
            }
        }
        for (const auto& function : module.functions) {
            append(function.symbolId, semantic::SymbolKind::Function, function.name,
                module.name + "::" + function.name,
                function.debugInfo.sourceName,
                function.debugInfo.declaration.span);
        }
    }
    std::sort(symbols.begin(), symbols.end(), [](const auto& left, const auto& right) {
        if (left.location.path != right.location.path) return left.location.path < right.location.path;
        return left.location.range.span.start < right.location.range.span.start;
    });
    return symbols;
}

std::optional<DocumentSymbol> LanguageService::resolve(
    const std::string& path,
    text::LinePosition position) const {
    const auto token = tokenAt(path, position);
    if (!token || token->kind != syntax::SyntaxKind::IdentifierToken) return std::nullopt;
    auto definitions = allDefinitions();
    std::vector<DocumentSymbol> matches;
    for (const auto& symbol : definitions) {
        if (symbol.name == token->text) matches.push_back(symbol);
    }
    if (matches.empty()) return std::nullopt;
    if (matches.size() == 1) return matches.front();
    const auto offset = token->span.start;
    const DocumentSymbol* nearest = nullptr;
    for (const auto& match : matches) {
        if (match.location.path != path || match.location.range.span.start > offset) continue;
        if (!nearest || match.location.range.span.start > nearest->location.range.span.start) nearest = &match;
    }
    return nearest ? std::optional<DocumentSymbol>(*nearest) : std::optional<DocumentSymbol>(matches.front());
}

std::vector<LanguageDiagnostic> LanguageService::diagnostics(const std::string& path) {
    if (!contains(path)) return {};
    (void)build();
    std::vector<LanguageDiagnostic> output;
    for (const auto& item : result_.diagnostics.items()) {
        if (!item.sourceName.empty() && item.sourceName != path) continue;
        output.push_back({item.code, item.message, item.severity, {path, rangeFor(path, item.span)}});
    }
    return output;
}

std::vector<CompletionItem> LanguageService::completion(
    const std::string& path,
    text::LinePosition position) {
    (void)build();
    const auto token = tokenAt(path, position);
    const auto cursorOffset = token ? token->span.start : std::size_t{0};
    std::vector<CompletionItem> output;
    const std::vector<std::string> keywords = {
        "module", "import", "class", "struct", "enum", "static", "return",
        "if", "else", "while", "new", "this", "true", "false", "null",
        "bool", "int", "long", "double", "string", "handle", "void",
    };
    for (const auto& keyword : keywords) output.push_back({keyword, "keyword", semantic::SymbolKind::Module});
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& symbol : allDefinitions()) {
        if ((symbol.kind == semantic::SymbolKind::Local ||
             symbol.kind == semantic::SymbolKind::Parameter) &&
            (symbol.location.path != path ||
             symbol.location.range.span.start > cursorOffset)) {
            continue;
        }
        if (seen.emplace(symbol.name, symbol.detail).second) {
            output.push_back({symbol.name, symbol.detail, symbol.kind});
        }
    }
    std::sort(output.begin(), output.end(), [](const auto& left, const auto& right) {
        return left.label < right.label;
    });
    return output;
}

std::optional<HoverInfo> LanguageService::hover(
    const std::string& path,
    text::LinePosition position) {
    (void)build();
    const auto symbol = resolve(path, position);
    if (!symbol) return std::nullopt;
    return HoverInfo{symbolKindName(symbol->kind) + std::string{" "} +
        symbol->name + (symbol->detail.empty() ? "" : ": " + symbol->detail),
        symbol->location};
}

std::optional<Location> LanguageService::definition(
    const std::string& path,
    text::LinePosition position) {
    (void)build();
    const auto symbol = resolve(path, position);
    return symbol ? std::optional<Location>(symbol->location) : std::nullopt;
}

std::vector<Location> LanguageService::references(
    const std::string& path,
    text::LinePosition position,
    bool includeDefinition) {
    (void)build();
    const auto token = tokenAt(path, position);
    const auto symbol = resolve(path, position);
    if (!token || !symbol) return {};
    std::vector<Location> output;
    for (const auto& [documentPath, value] : documents_) {
        text::SourceText source(value.content, documentPath);
        diagnostics::DiagnosticBag diagnostics;
        syntax::Lexer lexer(source, diagnostics);
        for (const auto& candidate : lexer.lexAll()) {
            if (candidate.kind != syntax::SyntaxKind::IdentifierToken || candidate.text != token->text) continue;
            const auto range = rangeFor(documentPath, candidate.span);
            const bool definitionLocation = documentPath == symbol->location.path &&
                candidate.span.start == symbol->location.range.span.start;
            if (includeDefinition || !definitionLocation) output.push_back({documentPath, range});
        }
    }
    std::sort(output.begin(), output.end(), [](const auto& left, const auto& right) {
        if (left.path != right.path) return left.path < right.path;
        return left.range.span.start < right.range.span.start;
    });
    return output;
}

std::vector<TextEdit> LanguageService::rename(
    const std::string& path,
    text::LinePosition position,
    const std::string& newName) {
    if (!identifier(newName)) return {};
    std::vector<TextEdit> edits;
    for (const auto& location : references(path, position, true)) {
        edits.push_back({location, newName});
    }
    return edits;
}

std::vector<DocumentSymbol> LanguageService::documentSymbols(const std::string& path) {
    (void)build();
    std::vector<DocumentSymbol> output;
    for (const auto& symbol : allDefinitions()) {
        if (symbol.location.path == path &&
            symbol.kind != semantic::SymbolKind::Local &&
            symbol.kind != semantic::SymbolKind::Parameter) {
            output.push_back(symbol);
        }
    }
    return output;
}

const char* symbolKindName(semantic::SymbolKind kind) noexcept {
    switch (kind) {
    case semantic::SymbolKind::Module: return "module";
    case semantic::SymbolKind::Type: return "type";
    case semantic::SymbolKind::Field: return "field";
    case semantic::SymbolKind::EnumMember: return "enum-member";
    case semantic::SymbolKind::Function: return "function";
    case semantic::SymbolKind::Property: return "property";
    case semantic::SymbolKind::Parameter: return "parameter";
    case semantic::SymbolKind::Local: return "local";
    }
    return "symbol";
}

} // namespace realscript::tooling
