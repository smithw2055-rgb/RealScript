#pragma once

#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DebugInfo.h"
#include "realscript/syntax/Syntax.h"

#include <cstdint>
#include <optional>
#include <set>
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
    bool loadProjectConfigJson(
        const std::string& jsonText,
        const std::string& baseDir = {},
        bool merge = false);
    bool loadProjectConfigFile(const std::string& configPath, bool merge = false);

    std::size_t addWorkspaceRoot(const std::string& rootPath);
    void removeWorkspaceRoot(const std::string& rootPath);
    bool syncDiskFile(const std::string& path);
    bool removeDiskFile(const std::string& path);

    [[nodiscard]] bool contains(const std::string& path) const noexcept;
    [[nodiscard]] bool isOpenDocument(const std::string& path) const noexcept;
    [[nodiscard]] std::vector<std::string> documentPaths() const;
    [[nodiscard]] std::vector<std::string> openDocumentPaths() const;
    [[nodiscard]] std::vector<std::string> workspaceRoots() const;
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
    [[nodiscard]] std::vector<TextEdit> formatDocument(const std::string& path, std::size_t tabSize = 4);
    [[nodiscard]] std::vector<DocumentSymbol> workspaceSymbols(const std::string& query = {});

    [[nodiscard]] const compiler::BuildResult& build();
    [[nodiscard]] std::uint64_t publicApiFingerprint(const std::string& moduleName);
    [[nodiscard]] std::size_t lastRebuiltModuleCount() const noexcept {
        return lastRebuiltModuleCount_;
    }

private:
    struct Document {
        std::string content;
        std::int64_t version = 0;
        debug::SourceFileInfo fileInfo{};
        mutable std::vector<syntax::SyntaxToken> tokens;
        mutable bool hasTokens = false;
    };

    static void initDocument(
        const std::string& path,
        std::string content,
        std::int64_t version,
        Document& doc);
    [[nodiscard]] const std::vector<syntax::SyntaxToken>& documentTokens(
        const std::string& path,
        const Document& doc) const;
    [[nodiscard]] const std::vector<DocumentSymbol>& allDefinitionsRef() const;

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

    [[nodiscard]] static std::string normalizeDocPath(const std::string& rawPath);
    [[nodiscard]] bool reloadDiskFileIntoWorkspace(const std::string& normalizedPath);

    std::unordered_map<std::string, Document> documents_;
    std::unordered_map<std::string, Document> workspaceDocuments_;
    std::unordered_map<std::string, Document> ambientDocuments_;
    std::set<std::string> workspaceRoots_;
    compiler::BuildResult result_;
    compiler::BuildSnapshot snapshot_;
    struct DocHeaderInfo {
        std::string moduleName;
        std::set<std::string> imports;
        bool eligibleForSubgraph = false;
    };
    static DocHeaderInfo parseDocHeader(const Document& doc);
    void recordFullBuildGraph();
    bool tryIncrementalSubgraphRebuild();

    std::unordered_map<std::string, DocHeaderInfo> docHeaders_;
    std::unordered_map<std::string, std::set<std::string>> moduleToPaths_;
    std::unordered_map<std::string, std::set<std::string>> moduleImports_;
    std::unordered_map<std::string, std::set<std::string>> reverseImports_;
    std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> moduleSymbolSlices_;
    std::unordered_map<std::string, std::uint64_t> modulePublicFingerprints_;
    std::set<std::string> dirtyPaths_;
    std::size_t lastRebuiltModuleCount_ = 0;
    bool structuralDirty_ = true;

    mutable std::vector<DocumentSymbol> cachedDefinitions_;
    mutable std::unordered_map<semantic::SymbolId, std::size_t> cachedDefById_;
    mutable std::unordered_map<std::string, std::vector<std::size_t>> cachedOccByPath_;
    mutable std::unordered_map<std::string, std::vector<LanguageDiagnostic>> cachedDiagnosticsByPath_;
    mutable bool hasCachedDefinitions_ = false;
    mutable bool hasCachedDiagnostics_ = false;
    bool hasSnapshot_ = false;
    bool dirty_ = true;
};

[[nodiscard]] const char* symbolKindName(semantic::SymbolKind kind) noexcept;

} // namespace realscript::tooling
