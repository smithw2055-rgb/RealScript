#pragma once

#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DebugInfo.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace realscript::tooling {

struct Location {
    std::string path;
    debug::SourceRange range;
};

struct LanguageDiagnostic {
    std::string code;
    std::string message;
    diagnostics::DiagnosticSeverity severity = diagnostics::DiagnosticSeverity::Error;
    Location location;
};

struct CompletionItem {
    std::string label;
    std::string detail;
    semantic::SymbolKind kind = semantic::SymbolKind::Local;
};

struct HoverInfo {
    std::string contents;
    Location location;
};

struct DocumentSymbol {
    semantic::SymbolId id = 0;
    semantic::SymbolKind kind = semantic::SymbolKind::Local;
    std::string name;
    std::string detail;
    Location location;
};

struct TextEdit {
    Location location;
    std::string replacement;
};

struct ContentChange {
    std::optional<debug::SourceRange> range;
    std::string text;
};

struct ParameterInfo {
    std::string label;
    std::string type;
};

struct SignatureInfo {
    std::string label;
    std::string documentation;
    std::vector<ParameterInfo> parameters;
};

struct SignatureHelp {
    std::vector<SignatureInfo> signatures;
    std::size_t activeSignature = 0;
    std::size_t activeParameter = 0;
};

struct SemanticToken {
    std::size_t line = 0;
    std::size_t startChar = 0;
    std::size_t length = 0;
    std::uint32_t tokenType = 0;
    std::uint32_t tokenModifiers = 0;
};

struct InlayHint {
    text::LinePosition position{};
    std::string label;
    std::uint32_t kind = 1; // 1 = Type, 2 = Parameter
};

class LanguageService {
public:
    void open(std::string path, std::string content, std::int64_t version = 1);
    void update(std::string path, std::string content, std::int64_t version);
    bool applyContentChanges(
        const std::string& path,
        const std::vector<ContentChange>& changes,
        std::int64_t version);
    [[nodiscard]] std::string documentContent(const std::string& path) const;
    void close(const std::string& path);

    void setAmbientSources(std::vector<compiler::SourceFile> sources);
    void addAmbientSource(compiler::SourceFile source);
    void clearAmbientSources();
    bool loadProjectConfigJson(const std::string& jsonText);

    [[nodiscard]] bool contains(const std::string& path) const noexcept;
    [[nodiscard]] std::vector<std::string> documentPaths() const;
    [[nodiscard]] std::vector<LanguageDiagnostic> diagnostics(const std::string& path);
    [[nodiscard]] std::vector<CompletionItem> completion(
        const std::string& path,
        text::LinePosition position);
    [[nodiscard]] std::optional<SignatureHelp> signatureHelp(
        const std::string& path,
        text::LinePosition position);
    [[nodiscard]] std::optional<HoverInfo> hover(
        const std::string& path,
        text::LinePosition position);
    [[nodiscard]] std::optional<Location> definition(
        const std::string& path,
        text::LinePosition position);
    [[nodiscard]] std::vector<Location> references(
        const std::string& path,
        text::LinePosition position,
        bool includeDefinition = true);
    [[nodiscard]] std::vector<TextEdit> rename(
        const std::string& path,
        text::LinePosition position,
        const std::string& newName);
    [[nodiscard]] std::vector<DocumentSymbol> documentSymbols(const std::string& path);
    [[nodiscard]] std::vector<SemanticToken> semanticTokens(const std::string& path);
    [[nodiscard]] std::vector<std::uint32_t> semanticTokensEncoded(const std::string& path);
    [[nodiscard]] std::vector<InlayHint> inlayHints(const std::string& path);

    [[nodiscard]] const compiler::BuildResult& build();

private:
    struct Document {
        std::string content;
        std::int64_t version = 0;
    };

    void rebuild();
    [[nodiscard]] const Document* document(const std::string& path) const noexcept;
    [[nodiscard]] std::optional<syntax::SyntaxToken> tokenAt(
        const std::string& path,
        text::LinePosition position) const;
    [[nodiscard]] std::vector<DocumentSymbol> allDefinitions() const;
    [[nodiscard]] std::optional<DocumentSymbol> resolve(
        const std::string& path,
        text::LinePosition position) const;
    [[nodiscard]] debug::SourceRange rangeFor(
        const std::string& path,
        text::TextSpan span) const;

    std::unordered_map<std::string, Document> documents_;
    std::unordered_map<std::string, Document> ambientDocuments_;
    compiler::BuildResult result_;
    compiler::BuildSnapshot snapshot_;
    bool hasSnapshot_ = false;
    bool dirty_ = true;
};

[[nodiscard]] const char* symbolKindName(semantic::SymbolKind kind) noexcept;

} // namespace realscript::tooling
