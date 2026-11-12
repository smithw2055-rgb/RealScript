#include "realscript/tooling/LanguageService.h"

#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"
#include "realscript/tooling/Json.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
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

void LanguageService::initDocument(
    const std::string& path,
    std::string content,
    std::int64_t version,
    Document& doc) {
    doc.content = std::move(content);
    doc.version = version;
    doc.fileInfo.id = 0;
    doc.fileInfo.path = path;
    doc.fileInfo.lineStarts.clear();
    doc.fileInfo.lineStarts.push_back(0);
    for (std::size_t offset = 0; offset < doc.content.size(); ++offset) {
        if (doc.content[offset] == '\n') {
            doc.fileInfo.lineStarts.push_back(static_cast<std::uint32_t>(offset + 1));
        }
    }
    doc.tokens.clear();
    doc.hasTokens = false;
}

const std::vector<syntax::SyntaxToken>& LanguageService::documentTokens(
    const std::string& path,
    const Document& doc) const {
    if (!doc.hasTokens) {
        text::SourceText source(doc.content, path);
        diagnostics::DiagnosticBag diagnostics;
        syntax::Lexer lexer(source, diagnostics);
        doc.tokens = lexer.lexAll();
        doc.hasTokens = true;
    }
    return doc.tokens;
}

LanguageService::DocHeaderInfo LanguageService::parseDocHeader(const Document& doc) {
    DocHeaderInfo info;
    const auto& s = doc.content;
    info.eligibleForSubgraph = true;
    std::size_t pos = 0;
    const auto skipWsAndComments = [&]() {
        while (pos < s.size()) {
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
            if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '/') {
                pos += 2;
                while (pos < s.size() && s[pos] != '\n') ++pos;
                continue;
            }
            if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') {
                pos += 2;
                while (pos + 1 < s.size() && !(s[pos] == '*' && s[pos + 1] == '/')) ++pos;
                if (pos + 1 < s.size()) pos += 2;
                continue;
            }
            break;
        }
    };
    const auto readWord = [&]() -> std::string {
        skipWsAndComments();
        std::size_t start = pos;
        while (pos < s.size() &&
               (std::isalnum(static_cast<unsigned char>(s[pos])) || s[pos] == '_' || s[pos] == '.')) {
            ++pos;
        }
        return s.substr(start, pos - start);
    };
    while (pos < s.size()) {
        skipWsAndComments();
        if (pos >= s.size()) break;
        if (s[pos] == '[') {
            int bracketDepth = 1;
            ++pos;
            bool inString = false;
            while (pos < s.size() && bracketDepth > 0) {
                if (s[pos] == '"' && (pos == 0 || s[pos - 1] != '\\')) {
                    inString = !inString;
                } else if (!inString) {
                    if (s[pos] == '[') ++bracketDepth;
                    else if (s[pos] == ']') --bracketDepth;
                }
                ++pos;
            }
            continue;
        }
        const std::size_t saved = pos;
        const auto kw = readWord();
        if (kw == "module") {
            const auto mName = readWord();
            skipWsAndComments();
            if (pos < s.size() && s[pos] == ';' && !mName.empty()) {
                ++pos;
                info.moduleName = mName;
                continue;
            }
            pos = saved;
            break;
        } else if (kw == "import") {
            const auto iName = readWord();
            skipWsAndComments();
            if (pos < s.size() && s[pos] == ';' && !iName.empty()) {
                ++pos;
                info.imports.insert(iName);
                continue;
            }
            pos = saved;
            break;
        } else {
            break;
        }
    }
    if (info.moduleName.empty()) {
        info.moduleName = "$global";
        info.eligibleForSubgraph = false;
    }
    return info;
}

namespace {

bool endsWithIgnoreCase(std::string_view text, std::string_view lowerSuffix) noexcept {
    if (text.size() < lowerSuffix.size()) return false;
    const std::size_t offset = text.size() - lowerSuffix.size();
    for (std::size_t i = 0; i < lowerSuffix.size(); ++i) {
        char ch = text[offset + i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (ch != lowerSuffix[i]) return false;
    }
    return true;
}

bool hasRealScriptExtension(const std::filesystem::path& p) {
    const std::string filename = p.filename().string();
    return endsWithIgnoreCase(filename, ".d.rscript") ||
        endsWithIgnoreCase(filename, ".rscript");
}

bool isDeclarationStubPath(const std::string& path) noexcept {
    return endsWithIgnoreCase(path, ".d.rscript");
}

bool isAcceptedEditorDocumentPath(const std::string& normalizedPath) {
    if (normalizedPath.empty()) return false;
    if (normalizedPath.rfind("untitled:", 0) == 0 ||
        normalizedPath.rfind("inmemory:", 0) == 0) {
        const auto colon = normalizedPath.find(':');
        const std::string tail = normalizedPath.substr(colon + 1);
        const auto slash = tail.find_last_of('/');
        const std::string base = (slash == std::string::npos) ? tail : tail.substr(slash + 1);
        if (base.find('.') == std::string::npos) {
            return !base.empty();
        }
        return endsWithIgnoreCase(base, ".d.rscript") ||
            endsWithIgnoreCase(base, ".rscript");
    }
    if (hasRealScriptExtension(std::filesystem::path(normalizedPath))) {
        return true;
    }
    return normalizedPath.find('/') == std::string::npos &&
        normalizedPath.find('.') == std::string::npos;
}

bool shouldSkipDirectoryName(const std::string& dirName) noexcept {
    if (dirName.empty() || dirName[0] == '.') return true;
    if (dirName == "build" || dirName == "out" || dirName == "dist" ||
        dirName == "target" || dirName == "node_modules" ||
        dirName == "CMakeFiles" || dirName == "jit-cache" ||
        dirName == "jit-benchmark-cache") {
        return true;
    }
    if (dirName.rfind("build-", 0) == 0 || dirName.rfind("build_", 0) == 0 ||
        dirName.rfind("cmake-build-", 0) == 0) {
        return true;
    }
    return false;
}

bool readFileContents(const std::filesystem::path& filePath, std::string& outText) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(filePath, ec) || ec) return false;
    std::ifstream in(filePath, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    outText = ss.str();
    return true;
}

bool isPathWithinRoot(const std::string& filePath, const std::string& rootPath) noexcept {
    if (rootPath.empty() || filePath.size() < rootPath.size()) return false;
    if (filePath.compare(0, rootPath.size(), rootPath) != 0) return false;
    if (filePath.size() == rootPath.size()) return true;
    return rootPath.back() == '/' || filePath[rootPath.size()] == '/';
}

} // namespace

std::string LanguageService::normalizeDocPath(const std::string& rawPath) {
    if (rawPath.empty()) return {};
    if (rawPath.rfind("untitled:", 0) == 0 ||
        rawPath.rfind("inmemory:", 0) == 0) {
        std::string v = rawPath;
        for (char& ch : v) {
            if (ch == '\\') ch = '/';
        }
        return v;
    }
    std::string p = uriToPath(rawPath);
    for (char& ch : p) {
        if (ch == '\\') ch = '/';
    }
    if (p.size() >= 2 &&
        ((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) &&
        p[1] == ':') {
        if (p[0] >= 'a' && p[0] <= 'z') {
            p[0] = static_cast<char>(p[0] - 'a' + 'A');
        }
    }
    // Preserve simple virtual filenames without path traversal segments as-is
    if (p.find('/') == std::string::npos) {
        return p;
    }
    std::string normalized = std::filesystem::path(p).lexically_normal().generic_string();
    while (normalized.size() > 1 && normalized.back() == '/') {
        if (normalized.size() == 3 && normalized[1] == ':') break;
        normalized.pop_back();
    }
    return normalized;
}

bool LanguageService::reloadDiskFileIntoWorkspace(const std::string& normalizedPath) {
    if (normalizedPath.empty() ||
        normalizedPath.rfind("untitled:", 0) == 0 ||
        normalizedPath.rfind("inmemory:", 0) == 0) {
        return false;
    }
    const std::filesystem::path fsPath(normalizedPath);
    if (!hasRealScriptExtension(fsPath)) return false;
    std::string diskContent;
    if (!readFileContents(fsPath, diskContent)) {
        return false;
    }
    auto& wsDoc = workspaceDocuments_[normalizedPath];
    initDocument(normalizedPath, std::move(diskContent), 1, wsDoc);
    return true;
}

void LanguageService::open(std::string rawPath, std::string content, std::int64_t version) {
    const std::string path = normalizeDocPath(rawPath);
    if (!isAcceptedEditorDocumentPath(path)) {
        return;
    }
    const bool existedInOpen = (documents_.find(path) != documents_.end());
    const bool existedInWorkspace = (workspaceDocuments_.find(path) != workspaceDocuments_.end());
    auto& doc = documents_[path];
    initDocument(path, std::move(content), version, doc);
    if (!existedInOpen && !existedInWorkspace) {
        structuralDirty_ = true;
    } else {
        dirtyPaths_.insert(path);
    }
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
}

void LanguageService::update(std::string path, std::string content, std::int64_t version) {
    open(std::move(path), std::move(content), version);
}

bool LanguageService::applyContentChanges(
    const std::string& rawPath,
    const std::vector<ContentChange>& changes,
    std::int64_t version) {
    const std::string path = normalizeDocPath(rawPath);
    if (!isAcceptedEditorDocumentPath(path)) {
        return false;
    }
    auto it = documents_.find(path);
    if (it == documents_.end()) {
        if (changes.empty()) return false;
        std::string seedContent;
        if (const auto wsIt = workspaceDocuments_.find(path); wsIt != workspaceDocuments_.end()) {
            seedContent = wsIt->second.content;
            dirtyPaths_.insert(path);
        } else {
            structuralDirty_ = true;
        }
        initDocument(path, std::move(seedContent), version, documents_[path]);
        it = documents_.find(path);
    } else {
        dirtyPaths_.insert(path);
    }
    std::string content = std::move(it->second.content);

    const auto lineColToOffset = [](const std::string& text, std::size_t line1, std::size_t col1) -> std::size_t {
        std::size_t curLine = 1;
        std::size_t idx = 0;
        while (idx < text.size() && curLine < line1) {
            if (text[idx] == '\n') ++curLine;
            ++idx;
        }
        std::size_t curCol = 1;
        while (idx < text.size() && text[idx] != '\n' && text[idx] != '\r' && curCol < col1) {
            ++idx;
            ++curCol;
        }
        return idx;
    };

    for (const auto& change : changes) {
        if (!change.range.has_value()) {
            content = change.text;
        } else {
            const auto& rng = *change.range;
            const std::size_t startOff = lineColToOffset(
                content, rng.start.line, rng.start.column);
            const std::size_t endOff = std::max(
                startOff,
                lineColToOffset(content, rng.end.line, rng.end.column));
            content.replace(startOff, endOff - startOff, change.text);
        }
    }
    initDocument(path, std::move(content), version, it->second);
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
    return true;
}

std::string LanguageService::documentContent(const std::string& rawPath) const {
    const auto* doc = document(rawPath);
    return doc ? doc->content : std::string{};
}

void LanguageService::close(const std::string& rawPath) {
    const std::string path = normalizeDocPath(rawPath);
    if (!isAcceptedEditorDocumentPath(path)) {
        return;
    }
    documents_.erase(path);
    // If the closed file exists on disk (either in an indexed workspace or standalone file),
    // reload its saved on-disk version into workspaceDocuments_ so modules importing it never break,
    // and any unsaved buffer edits are cleanly reverted to disk state.
    if (!reloadDiskFileIntoWorkspace(path)) {
        bool underIndexedRoot = false;
        for (const auto& root : workspaceRoots_) {
            if (isPathWithinRoot(path, root)) {
                underIndexedRoot = true;
                break;
            }
        }
        if (underIndexedRoot) {
            std::error_code ec;
            if (!std::filesystem::exists(std::filesystem::path(path), ec) || ec) {
                workspaceDocuments_.erase(path);
            }
        }
    }
    structuralDirty_ = true;
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
}

void LanguageService::setAmbientSources(std::vector<compiler::SourceFile> sources) {
    ambientDocuments_.clear();
    for (auto& src : sources) {
        const std::string normPath = normalizeDocPath(src.path);
        auto& doc = ambientDocuments_[normPath];
        initDocument(normPath, std::move(src.content), 1, doc);
    }
    structuralDirty_ = true;
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
}

void LanguageService::addAmbientSource(compiler::SourceFile source) {
    const std::string normPath = normalizeDocPath(source.path);
    auto& doc = ambientDocuments_[normPath];
    initDocument(normPath, std::move(source.content), 1, doc);
    structuralDirty_ = true;
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
}

void LanguageService::clearAmbientSources() {
    ambientDocuments_.clear();
    structuralDirty_ = true;
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
}

bool LanguageService::loadProjectConfigJson(
    const std::string& jsonText,
    const std::string& baseDir,
    bool merge) {
    std::string error;
    const auto parsed = Json::parse(jsonText, error);
    if (!parsed || !parsed->isObject()) return false;

    const std::filesystem::path baseFsPath =
        baseDir.empty() ? std::filesystem::path{} : std::filesystem::path(normalizeDocPath(baseDir));

    const auto* ambientArr = parsed->find("ambientStubs");
    if (!ambientArr) {
        ambientArr = parsed->find("ambientSources");
    }
    if (!ambientArr) {
        ambientArr = parsed->find("generatedModules");
    }

    bool loadedAnySection = false;
    if (ambientArr && ambientArr->isArray()) {
        if (!merge) {
            ambientDocuments_.clear();
        }
        for (const auto& entry : ambientArr->arrayValue()) {
            if (entry.isString() && !baseFsPath.empty()) {
                const std::filesystem::path relPath(entry.stringValue());
                const std::filesystem::path fullPath = relPath.is_absolute() ? relPath : (baseFsPath / relPath);
                if (!hasRealScriptExtension(fullPath)) continue;
                std::string diskStub;
                if (readFileContents(fullPath, diskStub)) {
                    const std::string key = normalizeDocPath(fullPath.generic_string());
                    initDocument(key, std::move(diskStub), 1, ambientDocuments_[key]);
                }
                continue;
            }
            if (!entry.isObject()) continue;
            const auto* pathVal = entry.find("path");
            const auto* contentVal = entry.find("content");
            if (!pathVal || !pathVal->isString()) continue;

            std::string resolvedKey = pathVal->stringValue();
            if (!baseFsPath.empty()) {
                const std::filesystem::path entryFsPath(resolvedKey);
                const std::filesystem::path fullPath =
                    entryFsPath.is_absolute() ? entryFsPath : (baseFsPath / entryFsPath);
                if (!hasRealScriptExtension(fullPath)) continue;
                resolvedKey = normalizeDocPath(fullPath.generic_string());
                if (contentVal && contentVal->isString()) {
                    initDocument(resolvedKey, contentVal->stringValue(), 1, ambientDocuments_[resolvedKey]);
                } else {
                    std::string diskStub;
                    if (readFileContents(fullPath, diskStub)) {
                        initDocument(resolvedKey, std::move(diskStub), 1, ambientDocuments_[resolvedKey]);
                    }
                }
            } else if (contentVal && contentVal->isString()) {
                resolvedKey = normalizeDocPath(resolvedKey);
                if (!hasRealScriptExtension(std::filesystem::path(resolvedKey))) continue;
                initDocument(resolvedKey, contentVal->stringValue(), 1, ambientDocuments_[resolvedKey]);
            }
        }
        loadedAnySection = true;
    }

    if (const auto* sourceDirsArr = parsed->find("sourceDirs");
        sourceDirsArr && sourceDirsArr->isArray() && !baseFsPath.empty()) {
        for (const auto& dirItem : sourceDirsArr->arrayValue()) {
            if (!dirItem.isString()) continue;
            const std::filesystem::path subDir(dirItem.stringValue());
            const std::filesystem::path fullDir = subDir.is_absolute() ? subDir : (baseFsPath / subDir);
            std::error_code ec;
            if (std::filesystem::is_directory(fullDir, ec) && !ec) {
                (void)addWorkspaceRoot(fullDir.generic_string());
            }
        }
        loadedAnySection = true;
    }

    if (!loadedAnySection) return false;
    structuralDirty_ = true;
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
    return true;
}

bool LanguageService::loadProjectConfigFile(const std::string& configPath, bool merge) {
    const std::string normConfig = normalizeDocPath(configPath);
    if (normConfig.empty()) return false;
    const std::filesystem::path fsConfig(normConfig);
    std::string jsonText;
    if (!readFileContents(fsConfig, jsonText)) return false;
    const std::string parentDir = fsConfig.has_parent_path()
        ? fsConfig.parent_path().generic_string()
        : std::string{};
    return loadProjectConfigJson(jsonText, parentDir, merge);
}

std::size_t LanguageService::addWorkspaceRoot(const std::string& rawRootPath) {
    const std::string rootPath = normalizeDocPath(rawRootPath);
    if (rootPath.empty()) return 0;
    const std::filesystem::path fsRoot(rootPath);
    std::error_code ec;
    if (!std::filesystem::exists(fsRoot, ec) || ec || !std::filesystem::is_directory(fsRoot, ec)) {
        return 0;
    }

    workspaceRoots_.insert(rootPath);
    std::size_t indexedCount = 0;

    // 1. Auto-discover root realscript.project.json if present
    const std::filesystem::path rootConfig = fsRoot / "realscript.project.json";
    bool mergedAmbient = !ambientDocuments_.empty();
    if (std::filesystem::is_regular_file(rootConfig, ec) && !ec) {
        if (loadProjectConfigFile(rootConfig.generic_string(), mergedAmbient)) {
            mergedAmbient = true;
        }
    }

    // 2. Recursively scan workspace directory for .rscript / .d.rscript files and nested realscript.project.json
    std::filesystem::recursive_directory_iterator it(
        fsRoot,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        std::error_code entryEc;
        if (entry.is_directory(entryEc) && !entryEc) {
            const std::string dirName = entry.path().filename().string();
            if (shouldSkipDirectoryName(dirName)) {
                it.disable_recursion_pending();
            }
            it.increment(ec);
            continue;
        }
        if (entry.is_regular_file(entryEc) && !entryEc) {
            const auto& p = entry.path();
            const std::string filename = p.filename().string();
            if (filename == "realscript.project.json" && p != rootConfig) {
                (void)loadProjectConfigFile(p.generic_string(), true);
            } else if (hasRealScriptExtension(p)) {
                const std::string normFile = normalizeDocPath(p.generic_string());
                if (reloadDiskFileIntoWorkspace(normFile)) {
                    ++indexedCount;
                }
            }
        }
        it.increment(ec);
    }

    structuralDirty_ = true;
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
    return indexedCount;
}

void LanguageService::removeWorkspaceRoot(const std::string& rawRootPath) {
    const std::string rootPath = normalizeDocPath(rawRootPath);
    if (workspaceRoots_.erase(rootPath) == 0) return;

    for (auto it = workspaceDocuments_.begin(); it != workspaceDocuments_.end();) {
        if (!isPathWithinRoot(it->first, rootPath)) {
            ++it;
            continue;
        }
        bool coveredByRemainingRoot = false;
        for (const auto& otherRoot : workspaceRoots_) {
            if (isPathWithinRoot(it->first, otherRoot)) {
                coveredByRemainingRoot = true;
                break;
            }
        }
        if (!coveredByRemainingRoot) {
            it = workspaceDocuments_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = ambientDocuments_.begin(); it != ambientDocuments_.end();) {
        if (!isPathWithinRoot(it->first, rootPath)) {
            ++it;
            continue;
        }
        bool coveredByRemainingRoot = false;
        for (const auto& otherRoot : workspaceRoots_) {
            if (isPathWithinRoot(it->first, otherRoot)) {
                coveredByRemainingRoot = true;
                break;
            }
        }
        if (!coveredByRemainingRoot) {
            it = ambientDocuments_.erase(it);
        } else {
            ++it;
        }
    }
    structuralDirty_ = true;
    dirty_ = true;
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
}

bool LanguageService::syncDiskFile(const std::string& rawPath) {
    const std::string path = normalizeDocPath(rawPath);
    if (path.empty()) return false;
    const std::filesystem::path fsPath(path);
    if (fsPath.filename() == "realscript.project.json") {
        return loadProjectConfigFile(path, !workspaceRoots_.empty() || !ambientDocuments_.empty());
    }
    const bool wasInWorkspace = (workspaceDocuments_.find(path) != workspaceDocuments_.end());
    if (!reloadDiskFileIntoWorkspace(path)) {
        return false;
    }
    // If not shadowed by an open editor buffer, invalidate build graph
    if (documents_.find(path) == documents_.end()) {
        if (wasInWorkspace) {
            dirtyPaths_.insert(path);
        } else {
            structuralDirty_ = true;
        }
        dirty_ = true;
        hasCachedDefinitions_ = false;
        hasCachedDiagnostics_ = false;
    }
    return true;
}

bool LanguageService::removeDiskFile(const std::string& rawPath) {
    const std::string path = normalizeDocPath(rawPath);
    const bool erased = (workspaceDocuments_.erase(path) > 0);
    if (erased && documents_.find(path) == documents_.end()) {
        structuralDirty_ = true;
        dirty_ = true;
        hasCachedDefinitions_ = false;
        hasCachedDiagnostics_ = false;
    }
    return erased;
}

bool LanguageService::contains(const std::string& rawPath) const noexcept {
    const std::string path = normalizeDocPath(rawPath);
    return documents_.find(path) != documents_.end() ||
        workspaceDocuments_.find(path) != workspaceDocuments_.end();
}

bool LanguageService::isOpenDocument(const std::string& rawPath) const noexcept {
    const std::string path = normalizeDocPath(rawPath);
    return documents_.find(path) != documents_.end();
}

std::vector<std::string> LanguageService::documentPaths() const {
    std::set<std::string> uniquePaths;
    for (const auto& [path, value] : workspaceDocuments_) {
        (void)value;
        uniquePaths.insert(path);
    }
    for (const auto& [path, value] : documents_) {
        (void)value;
        uniquePaths.insert(path);
    }
    return std::vector<std::string>(uniquePaths.begin(), uniquePaths.end());
}

std::vector<std::string> LanguageService::openDocumentPaths() const {
    std::vector<std::string> paths;
    paths.reserve(documents_.size());
    for (const auto& [path, value] : documents_) {
        (void)value;
        paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::vector<std::string> LanguageService::workspaceRoots() const {
    return std::vector<std::string>(workspaceRoots_.begin(), workspaceRoots_.end());
}

const compiler::BuildResult& LanguageService::build() {
    if (dirty_) rebuild();
    return result_;
}

std::uint64_t LanguageService::publicApiFingerprint(const std::string& moduleName) {
    if (dirty_) rebuild();
    const auto it = modulePublicFingerprints_.find(moduleName);
    return it != modulePublicFingerprints_.end() ? it->second : 0;
}

void LanguageService::recordFullBuildGraph() {
    docHeaders_.clear();
    moduleToPaths_.clear();
    moduleImports_.clear();
    reverseImports_.clear();
    moduleSymbolSlices_.clear();
    modulePublicFingerprints_.clear();
    lastRebuiltModuleCount_ = 0;
    for (const auto& bi : result_.buildInfo) {
        modulePublicFingerprints_[bi.name] = bi.publicFingerprint;
        if (!bi.reused) ++lastRebuiltModuleCount_;
    }

    std::unordered_set<std::string> activeStubModules;
    for (const auto& [path, doc] : workspaceDocuments_) {
        if (documents_.find(path) != documents_.end()) continue;
        auto hdr = parseDocHeader(doc);
        if (isDeclarationStubPath(path)) activeStubModules.insert(hdr.moduleName);
        moduleToPaths_[hdr.moduleName].insert(path);
        for (const auto& imp : hdr.imports) {
            moduleImports_[hdr.moduleName].insert(imp);
            reverseImports_[imp].insert(hdr.moduleName);
        }
        docHeaders_[path] = std::move(hdr);
    }
    for (const auto& [path, doc] : documents_) {
        auto hdr = parseDocHeader(doc);
        if (isDeclarationStubPath(path)) activeStubModules.insert(hdr.moduleName);
        moduleToPaths_[hdr.moduleName].insert(path);
        for (const auto& imp : hdr.imports) {
            moduleImports_[hdr.moduleName].insert(imp);
            reverseImports_[imp].insert(hdr.moduleName);
        }
        docHeaders_[path] = std::move(hdr);
    }
    for (const auto& [path, doc] : ambientDocuments_) {
        if (documents_.find(path) != documents_.end() ||
            workspaceDocuments_.find(path) != workspaceDocuments_.end()) {
            continue;
        }
        auto hdr = parseDocHeader(doc);
        if (isDeclarationStubPath(path) && activeStubModules.find(hdr.moduleName) != activeStubModules.end()) {
            continue;
        }
        moduleToPaths_[hdr.moduleName].insert(path);
        for (const auto& imp : hdr.imports) {
            moduleImports_[hdr.moduleName].insert(imp);
            reverseImports_[imp].insert(hdr.moduleName);
        }
        docHeaders_[path] = std::move(hdr);
    }

    // Compute per-module symbol slices in result_.symbols
    for (const auto& [modName, paths] : moduleToPaths_) {
        (void)paths;
        moduleSymbolSlices_[modName] = {result_.symbols.size(), 0};
    }
    std::unordered_set<std::string> seenNonEmptyModules;
    std::size_t idx = 0;
    while (idx < result_.symbols.size()) {
        const auto& srcPath = result_.symbols[idx].sourceName;
        const auto hdrIt = docHeaders_.find(srcPath);
        if (hdrIt == docHeaders_.end()) {
            ++idx;
            continue;
        }
        const std::string& modName = hdrIt->second.moduleName;
        const std::size_t start = idx;
        while (idx < result_.symbols.size()) {
            const auto& p = result_.symbols[idx].sourceName;
            const auto it2 = docHeaders_.find(p);
            if (it2 == docHeaders_.end() || it2->second.moduleName != modName) break;
            ++idx;
        }
        if (seenNonEmptyModules.insert(modName).second) {
            moduleSymbolSlices_[modName] = {start, idx - start};
        } else {
            // Non-contiguous symbols for a module: disable fast slice replacement
            moduleSymbolSlices_.clear();
            break;
        }
    }
    dirtyPaths_.clear();
    structuralDirty_ = false;
}

bool LanguageService::tryIncrementalSubgraphRebuild() {
    if (structuralDirty_ || !hasSnapshot_ || dirtyPaths_.empty() ||
        moduleSymbolSlices_.empty()) {
        return false;
    }

    std::set<std::string> dirtyModules;
    for (const auto& path : dirtyPaths_) {
        const auto* docPtr = document(path);
        const auto oldHdrIt = docHeaders_.find(path);
        if (!docPtr || oldHdrIt == docHeaders_.end()) return false;
        const auto newHdr = parseDocHeader(*docPtr);
        if (!newHdr.eligibleForSubgraph ||
            newHdr.moduleName != oldHdrIt->second.moduleName ||
            newHdr.imports != oldHdrIt->second.imports) {
            return false;
        }
        dirtyModules.insert(newHdr.moduleName);
    }
    if (dirtyModules.size() != 1) return false;
    const std::string& dirtyMod = *dirtyModules.begin();
    if (moduleSymbolSlices_.find(dirtyMod) == moduleSymbolSlices_.end()) return false;

    // Collect transitive imports of dirtyMod
    std::set<std::string> subgraphModules;
    std::vector<std::string> queue = {dirtyMod};
    while (!queue.empty()) {
        auto curr = std::move(queue.back());
        queue.pop_back();
        if (curr == "RealScript.Math" || curr == "RealScript.Fiber" ||
            curr == "RealScript.Async" || curr == "RealScript.Resource") {
            continue;
        }
        if (!subgraphModules.insert(curr).second) continue;
        if (curr != dirtyMod && snapshot_.modules.find(curr) == snapshot_.modules.end()) {
            return false;
        }
        if (const auto impIt = moduleImports_.find(curr); impIt != moduleImports_.end()) {
            for (const auto& imp : impIt->second) queue.push_back(imp);
        }
    }

    // If subgraph is smaller than total workspace, run subgraph Compilation
    if (subgraphModules.size() >= moduleToPaths_.size()) {
        return false;
    }

    compiler::Compilation subCompilation;
    std::set<std::string> dirtyModPaths;
    for (const auto& modName : subgraphModules) {
        const auto pathsIt = moduleToPaths_.find(modName);
        if (pathsIt == moduleToPaths_.end()) return false;
        for (const auto& p : pathsIt->second) {
            const auto* doc = document(p);
            if (!doc) return false;
            subCompilation.addSource({p, doc->content});
            if (modName == dirtyMod) dirtyModPaths.insert(p);
        }
    }

    auto subResult = subCompilation.build(&snapshot_);

    const compiler::ModuleBuildInfo* newBuildInfo = nullptr;
    for (const auto& sbi : subResult.buildInfo) {
        if (sbi.name == dirtyMod) {
            newBuildInfo = &sbi;
        } else if (!sbi.reused) {
            // An imported module was mutated (e.g., cross-module generic specialization):
            // fall back to full workspace rebuild for consistency.
            return false;
        }
    }
    if (!newBuildInfo) return false;

    // If other modules import dirtyMod, verify dirtyMod's publicFingerprint did not change
    const auto revIt = reverseImports_.find(dirtyMod);
    const bool hasDownstreamImporters =
        (revIt != reverseImports_.end() && !revIt->second.empty());
    if (hasDownstreamImporters) {
        const auto oldFpIt = modulePublicFingerprints_.find(dirtyMod);
        const auto snapOld = snapshot_.modules.find(dirtyMod);
        if (oldFpIt == modulePublicFingerprints_.end() ||
            snapOld == snapshot_.modules.end() ||
            oldFpIt->second != newBuildInfo->publicFingerprint) {
            return false;
        }
        if (const auto snapNew = subResult.snapshot.modules.find(dirtyMod);
            snapNew != subResult.snapshot.modules.end()) {
            if (snapOld->second.module.languageMetadata.genericInstantiations.size() !=
                snapNew->second.module.languageMetadata.genericInstantiations.size()) {
                return false;
            }
        }
    }

    // 1. Update snapshot_.modules[dirtyMod] and modulePublicFingerprints_[dirtyMod]
    modulePublicFingerprints_[dirtyMod] = newBuildInfo->publicFingerprint;
    if (auto snapNew = subResult.snapshot.modules.find(dirtyMod);
        snapNew != subResult.snapshot.modules.end()) {
        snapshot_.modules[dirtyMod] = std::move(snapNew->second);
    } else if (!hasDownstreamImporters) {
        snapshot_.modules.erase(dirtyMod);
    }

    // 2. Update result_.modules for dirtyMod
    const mir::Module* newMirMod = nullptr;
    for (auto& m : subResult.modules) {
        if (m.name == dirtyMod) {
            newMirMod = &m;
            break;
        }
    }
    if (newMirMod) {
        bool replaced = false;
        for (auto& existingMod : result_.modules) {
            if (existingMod.name == dirtyMod) {
                existingMod = *newMirMod;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result_.modules.push_back(*newMirMod);
        }
    } else {
        result_.modules.erase(
            std::remove_if(
                result_.modules.begin(),
                result_.modules.end(),
                [&](const mir::Module& m) { return m.name == dirtyMod; }),
            result_.modules.end());
    }

    // 3. Update result_.buildInfo
    lastRebuiltModuleCount_ = 1;
    for (auto& bi : result_.buildInfo) {
        if (bi.name != dirtyMod) {
            bi.reused = true;
        } else {
            bi = *newBuildInfo;
        }
    }

    // 4. Update result_.diagnostics (keep diagnostics from non-dirty files, replace dirtyMod diagnostics)
    diagnostics::DiagnosticBag mergedDiags;
    for (const auto& item : result_.diagnostics.items()) {
        if (dirtyModPaths.find(item.sourceName) == dirtyModPaths.end()) {
            mergedDiags.report(item.code, item.message, item.span, item.severity, item.sourceName);
        }
    }
    for (const auto& item : subResult.diagnostics.items()) {
        const std::string effectiveSource =
            (!item.sourceName.empty() || dirtyModPaths.size() != 1)
                ? item.sourceName
                : *dirtyModPaths.begin();
        if (effectiveSource.empty() || dirtyModPaths.find(effectiveSource) != dirtyModPaths.end()) {
            mergedDiags.report(item.code, item.message, item.span, item.severity, effectiveSource);
        }
    }
    result_.diagnostics = std::move(mergedDiags);

    // 4b. Update nativeAttributes, nativeSequences, nativeInterfaces, nativeGenericInstantiations for dirtyMod
    const std::string modPrefix = dirtyMod + "::";
    result_.nativeAttributes.erase(
        std::remove_if(result_.nativeAttributes.begin(), result_.nativeAttributes.end(),
            [&](const compiler::LanguageAttributeRecord& attr) {
                return attr.target == dirtyMod || attr.target.rfind(modPrefix, 0) == 0;
            }),
        result_.nativeAttributes.end());
    for (const auto& attr : subResult.nativeAttributes) {
        if (attr.target == dirtyMod || attr.target.rfind(modPrefix, 0) == 0) {
            result_.nativeAttributes.push_back(attr);
        }
    }

    result_.nativeSequences.erase(
        std::remove_if(result_.nativeSequences.begin(), result_.nativeSequences.end(),
            [&](const compiler::LanguageSequenceRecord& seq) {
                return seq.typeName.rfind(modPrefix, 0) == 0;
            }),
        result_.nativeSequences.end());
    for (const auto& seq : subResult.nativeSequences) {
        if (seq.typeName.rfind(modPrefix, 0) == 0) {
            result_.nativeSequences.push_back(seq);
        }
    }

    result_.nativeInterfaces.erase(
        std::remove_if(result_.nativeInterfaces.begin(), result_.nativeInterfaces.end(),
            [&](const compiler::LanguageInterfaceImplementation& impl) {
                return impl.typeName.rfind(modPrefix, 0) == 0;
            }),
        result_.nativeInterfaces.end());
    for (const auto& impl : subResult.nativeInterfaces) {
        if (impl.typeName.rfind(modPrefix, 0) == 0) {
            result_.nativeInterfaces.push_back(impl);
        }
    }

    if (!result_.nativeGenericInstantiations.empty() ||
        !subResult.nativeGenericInstantiations.empty()) {
        std::vector<compiler::LanguageGenericInstantiation> mergedGenerics;
        for (const auto& m : result_.modules) {
            if (m.name == dirtyMod) continue;
            mergedGenerics.insert(
                mergedGenerics.end(),
                m.languageMetadata.genericInstantiations.begin(),
                m.languageMetadata.genericInstantiations.end());
        }
        mergedGenerics.insert(
            mergedGenerics.end(),
            subResult.nativeGenericInstantiations.begin(),
            subResult.nativeGenericInstantiations.end());
        std::stable_sort(
            mergedGenerics.begin(),
            mergedGenerics.end(),
            [](const auto& left, const auto& right) {
                if (left.generatedName != right.generatedName) {
                    return left.generatedName < right.generatedName;
                }
                if (left.genericName != right.genericName) {
                    return left.genericName < right.genericName;
                }
                return left.arguments < right.arguments;
            });
        mergedGenerics.erase(
            std::unique(
                mergedGenerics.begin(),
                mergedGenerics.end(),
                [](const auto& left, const auto& right) {
                    return left.generatedName == right.generatedName &&
                        left.genericName == right.genericName &&
                        left.arguments == right.arguments;
                }),
            mergedGenerics.end());
        result_.nativeGenericInstantiations = std::move(mergedGenerics);
    }

    // 5. Update result_.symbols for dirtyMod in O(|dirtyMod symbols|) by moving dirtyMod slice to tail if needed
    std::vector<semantic::SymbolOccurrence> newModSymbols;
    for (auto& occ : subResult.symbols) {
        if (dirtyModPaths.find(occ.sourceName) != dirtyModPaths.end()) {
            newModSymbols.push_back(std::move(occ));
        }
    }
    auto slice = moduleSymbolSlices_[dirtyMod];
    if (slice.second == 0) {
        slice.first = result_.symbols.size();
    } else if (slice.first + slice.second < result_.symbols.size()) {
        // Move dirtyMod slice to the end once so subsequent edits are O(1) tail resize
        const std::size_t oldStart = slice.first;
        const std::size_t oldCount = slice.second;
        result_.symbols.erase(
            result_.symbols.begin() + static_cast<std::ptrdiff_t>(oldStart),
            result_.symbols.begin() + static_cast<std::ptrdiff_t>(oldStart + oldCount));
        for (auto& [mName, mSlice] : moduleSymbolSlices_) {
            if (mName != dirtyMod && mSlice.second > 0 && mSlice.first > oldStart) {
                mSlice.first -= oldCount;
            }
        }
        slice.first = result_.symbols.size();
        slice.second = 0;
    }
    result_.symbols.resize(slice.first);
    slice.second = newModSymbols.size();
    result_.symbols.insert(
        result_.symbols.end(),
        std::make_move_iterator(newModSymbols.begin()),
        std::make_move_iterator(newModSymbols.end()));
    moduleSymbolSlices_[dirtyMod] = slice;

    dirtyPaths_.clear();
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
    dirty_ = false;
    return true;
}

void LanguageService::rebuild() {
    if (tryIncrementalSubgraphRebuild()) {
        return;
    }
    compiler::Compilation compilation;
    const auto allProjectPaths = documentPaths();
    std::unordered_set<std::string> activeStubModules;
    for (const auto& path : allProjectPaths) {
        if (isDeclarationStubPath(path)) {
            if (const auto* d = document(path)) {
                activeStubModules.insert(parseDocHeader(*d).moduleName);
            }
        }
    }

    std::vector<std::string> ambientPaths;
    ambientPaths.reserve(ambientDocuments_.size());
    for (const auto& [path, docVal] : ambientDocuments_) {
        if (documents_.find(path) != documents_.end() ||
            workspaceDocuments_.find(path) != workspaceDocuments_.end()) {
            continue;
        }
        if (isDeclarationStubPath(path) &&
            activeStubModules.find(parseDocHeader(docVal).moduleName) != activeStubModules.end()) {
            continue;
        }
        ambientPaths.push_back(path);
    }
    std::sort(ambientPaths.begin(), ambientPaths.end());
    for (const auto& path : ambientPaths) {
        compilation.addSource({path, ambientDocuments_.at(path).content});
    }

    for (const auto& path : allProjectPaths) {
        if (const auto* d = document(path)) {
            compilation.addSource({path, d->content});
        }
    }
    result_ = compilation.build(hasSnapshot_ ? &snapshot_ : nullptr);
    if (!result_.snapshot.modules.empty()) {
        for (auto& [modName, cachedMod] : result_.snapshot.modules) {
            snapshot_.modules[modName] = std::move(cachedMod);
        }
        hasSnapshot_ = true;
    }
    recordFullBuildGraph();
    hasCachedDefinitions_ = false;
    hasCachedDiagnostics_ = false;
    dirty_ = false;
}

const LanguageService::Document* LanguageService::document(const std::string& rawPath) const noexcept {
    const std::string path = normalizeDocPath(rawPath);
    if (const auto found = documents_.find(path); found != documents_.end()) {
        return &found->second;
    }
    if (const auto wsFound = workspaceDocuments_.find(path); wsFound != workspaceDocuments_.end()) {
        return &wsFound->second;
    }
    const auto ambientFound = ambientDocuments_.find(path);
    return ambientFound == ambientDocuments_.end() ? nullptr : &ambientFound->second;
}

debug::SourceRange LanguageService::rangeFor(
    const std::string& path,
    text::TextSpan span) const {
    const auto* value = document(path);
    if (!value) return {};
    return debug::makeSourceRange(value->fileInfo, span);
}

std::optional<syntax::SyntaxToken> LanguageService::tokenAt(
    const std::string& path,
    text::LinePosition position) const {
    const auto* value = document(path);
    if (!value) return std::nullopt;
    const auto& lineStarts = value->fileInfo.lineStarts;
    const std::size_t offset = position.line < lineStarts.size()
        ? std::min(static_cast<std::size_t>(lineStarts[position.line]) + position.column, value->content.size())
        : value->content.size();
    for (const auto& token : documentTokens(path, *value)) {
        if (token.kind == syntax::SyntaxKind::EndOfFileToken) continue;
        if (offset >= token.span.start && offset <= token.span.end()) return token;
    }
    return std::nullopt;
}

const std::vector<DocumentSymbol>& LanguageService::allDefinitionsRef() const {
    if (hasCachedDefinitions_) return cachedDefinitions_;
    cachedDefinitions_.clear();
    cachedDefById_.clear();
    cachedOccByPath_.clear();
    std::unordered_set<semantic::SymbolId> seen;
    std::unordered_set<std::string> generatedNames;
    for (const auto& module : result_.modules) {
        for (const auto& instance :
             module.languageMetadata.genericInstantiations) {
            generatedNames.insert(instance.generatedName);
        }
    }
    auto append = [&](semantic::SymbolId id, semantic::SymbolKind kind,
                      std::string name, std::string detail,
                      const std::string& path, text::TextSpan span) {
        if (id == 0 || path.empty() ||
            document(path) == nullptr ||
            !identifier(name) || generatedNames.find(name) != generatedNames.end() ||
            !seen.insert(id).second) {
            return;
        }
        cachedDefinitions_.push_back({id, kind, std::move(name), std::move(detail), {path, rangeFor(path, span)}});
    };

    for (std::size_t occIdx = 0; occIdx < result_.symbols.size(); ++occIdx) {
        const auto& occurrence = result_.symbols[occIdx];
        if (!occurrence.sourceName.empty()) {
            cachedOccByPath_[occurrence.sourceName].push_back(occIdx);
        }
        if (occurrence.definition) {
            append(occurrence.id, occurrence.kind, occurrence.name, occurrence.detail,
                occurrence.sourceName, occurrence.span);
        }
    }
    for (const auto& module : result_.modules) {
        for (const auto& type : module.types) {
            if (type.synthetic) continue;
            append(type.id, semantic::SymbolKind::Type, type.name,
                semantic::canonicalTypeName(type), type.sourceName, type.declarationSpan);
            for (const auto& field : type.fields) {
                if (field.synthetic) continue;
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
                if (method.synthetic) continue;
                append(method.id, semantic::SymbolKind::Function, method.name,
                    functionDetail(method), method.sourceName, method.declarationSpan);
            }
            for (const auto& constructor : type.constructors) {
                if (constructor.synthetic) continue;
                append(constructor.id, semantic::SymbolKind::Function, type.name,
                    functionDetail(constructor), constructor.sourceName, constructor.declarationSpan);
            }
            if (type.staticConstructor && !type.staticConstructor->synthetic) {
                append(type.staticConstructor->id, semantic::SymbolKind::Function, type.name,
                    functionDetail(*type.staticConstructor), type.staticConstructor->sourceName,
                    type.staticConstructor->declarationSpan);
            }
        }
        for (const auto& function : module.functions) {
            append(function.symbolId, semantic::SymbolKind::Function, function.name,
                module.name + "::" + function.name,
                function.debugInfo.sourceName,
                function.debugInfo.declaration.span);
        }
    }
    std::sort(cachedDefinitions_.begin(), cachedDefinitions_.end(), [](const auto& left, const auto& right) {
        if (left.location.path != right.location.path) return left.location.path < right.location.path;
        return left.location.range.span.start < right.location.range.span.start;
    });
    for (std::size_t idx = 0; idx < cachedDefinitions_.size(); ++idx) {
        cachedDefById_[cachedDefinitions_[idx].id] = idx;
    }
    hasCachedDefinitions_ = true;
    return cachedDefinitions_;
}

std::vector<DocumentSymbol> LanguageService::allDefinitions() const {
    return allDefinitionsRef();
}

std::optional<DocumentSymbol> LanguageService::resolve(
    const std::string& rawPath,
    text::LinePosition position) const {
    const std::string path = normalizeDocPath(rawPath);
    const auto token = tokenAt(path, position);
    if (!token || token->kind != syntax::SyntaxKind::IdentifierToken) return std::nullopt;
    const auto& definitions = allDefinitionsRef();

    for (const auto& symbol : definitions) {
        if (symbol.location.path == path &&
            symbol.location.range.span.start == token->span.start &&
            symbol.location.range.span.length == token->span.length) {
            return symbol;
        }
    }

    semantic::SymbolId matchedId = 0;
    const semantic::SymbolOccurrence* matchedOcc = nullptr;
    if (const auto pathOccs = cachedOccByPath_.find(path); pathOccs != cachedOccByPath_.end()) {
        for (const std::size_t occIdx : pathOccs->second) {
            const auto& occ = result_.symbols[occIdx];
            if (occ.id == 0) continue;
            if (occ.span.start == token->span.start &&
                occ.span.length == token->span.length) {
                matchedId = occ.id;
                matchedOcc = &occ;
                if (occ.definition) break;
            }
        }
    }
    if (matchedId != 0) {
        if (const auto defIt = cachedDefById_.find(matchedId); defIt != cachedDefById_.end()) {
            return definitions[defIt->second];
        }
        for (const auto& occ : result_.symbols) {
            if (occ.id == matchedId && occ.definition && !occ.sourceName.empty()) {
                return DocumentSymbol{
                    occ.id,
                    occ.kind,
                    occ.name,
                    occ.detail,
                    {occ.sourceName, rangeFor(occ.sourceName, occ.span)}};
            }
        }
        if (matchedOcc) {
            return DocumentSymbol{
                matchedOcc->id,
                matchedOcc->kind,
                matchedOcc->name,
                matchedOcc->detail,
                {matchedOcc->sourceName, rangeFor(matchedOcc->sourceName, matchedOcc->span)}};
        }
    }

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

std::vector<LanguageDiagnostic> LanguageService::diagnostics(const std::string& rawPath) {
    const std::string path = normalizeDocPath(rawPath);
    if (!contains(path)) return {};
    (void)build();
    if (!hasCachedDiagnostics_) {
        cachedDiagnosticsByPath_.clear();
        const auto& items = result_.diagnostics.items();
        if (!items.empty()) {
            std::vector<std::string> projPaths;
            std::vector<std::string> openPaths;
            std::string primaryDoc;
            std::unordered_set<std::string> succeededModules;
            bool sourcelessContextInitialized = false;

            const auto ensureSourcelessContext = [&]() {
                if (sourcelessContextInitialized) return;
                sourcelessContextInitialized = true;
                projPaths = documentPaths();
                openPaths = openDocumentPaths();
                const auto& fallbackPool = openPaths.empty() ? projPaths : openPaths;
                primaryDoc = fallbackPool.empty() ? std::string{} : fallbackPool.front();
                for (const auto& m : result_.modules) {
                    succeededModules.insert(m.name);
                }
            };

            const auto inferOwnerForSourceless = [&](const diagnostics::Diagnostic& item, bool& outHasValidSpan) -> std::string {
                ensureSourcelessContext();
                outHasValidSpan = false;
                std::string quotedToken;
                const std::size_t q1 = item.message.find('\'');
                if (q1 != std::string::npos) {
                    const std::size_t q2 = item.message.find('\'', q1 + 1);
                    if (q2 != std::string::npos && q2 > q1 + 1) {
                        quotedToken = item.message.substr(q1 + 1, q2 - q1 - 1);
                    }
                }

                std::vector<std::string> spanTokenMatches;
                std::vector<std::string> failedModCandidates;
                for (const auto& candPath : projPaths) {
                    const auto* docPtr = document(candPath);
                    if (!docPtr) continue;
                    const auto hdrIt = docHeaders_.find(candPath);
                    const bool modFailed = (hdrIt == docHeaders_.end()) ||
                        (succeededModules.find(hdrIt->second.moduleName) == succeededModules.end());
                    if (!modFailed) continue;
                    if (item.span.end() <= docPtr->content.size()) {
                        failedModCandidates.push_back(candPath);
                        if (!quotedToken.empty() && item.span.length > 0) {
                            const std::string_view slice(
                                docPtr->content.data() + item.span.start,
                                item.span.length);
                            if (slice.find(quotedToken) != std::string_view::npos) {
                                spanTokenMatches.push_back(candPath);
                            }
                        }
                    }
                }
                if (spanTokenMatches.size() == 1) {
                    outHasValidSpan = true;
                    return spanTokenMatches.front();
                }
                if (failedModCandidates.size() == 1) {
                    outHasValidSpan = (item.span.length > 0);
                    return failedModCandidates.front();
                }
                return primaryDoc;
            };

            for (const auto& item : items) {
                if (!item.sourceName.empty()) {
                    const std::string itemPath = normalizeDocPath(item.sourceName);
                    cachedDiagnosticsByPath_[itemPath].push_back(
                        {item.code, item.message, item.severity, {itemPath, rangeFor(itemPath, item.span)}});
                } else {
                    bool validSpan = false;
                    const std::string ownerPath = inferOwnerForSourceless(item, validSpan);
                    if (!ownerPath.empty()) {
                        cachedDiagnosticsByPath_[ownerPath].push_back({
                            item.code,
                            item.message,
                            item.severity,
                            {ownerPath, validSpan ? rangeFor(ownerPath, item.span) : debug::SourceRange{}},
                        });
                    }
                }
            }
        }
        hasCachedDiagnostics_ = true;
    }
    const auto it = cachedDiagnosticsByPath_.find(path);
    return it != cachedDiagnosticsByPath_.end() ? it->second : std::vector<LanguageDiagnostic>{};
}

std::vector<CompletionItem> LanguageService::completion(
    const std::string& rawPath,
    text::LinePosition position) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    const auto token = tokenAt(path, position);
    const auto cursorOffset = token ? token->span.start : std::size_t{0};
    std::string requesterModule;
    semantic::SymbolId requesterTypeId = 0;
    std::unordered_map<semantic::SymbolId, const semantic::TypeSymbol*> types;
    for (const auto& module : result_.modules) {
        for (const auto& source : module.sourceFiles) {
            if (source.path == path) requesterModule = module.name;
        }
        for (const auto& type : module.types) {
            types[type.id] = &type;
            const auto containsCursor = [&](const semantic::FunctionSymbol& fn) {
                return fn.sourceName == path &&
                    cursorOffset >= fn.bodySpan.start &&
                    cursorOffset <= fn.bodySpan.end();
            };
            for (const auto& method : type.methods) {
                if (containsCursor(method)) requesterTypeId = type.id;
            }
            for (const auto& constructor : type.constructors) {
                if (containsCursor(constructor)) requesterTypeId = type.id;
            }
            if (type.staticConstructor && containsCursor(*type.staticConstructor)) {
                requesterTypeId = type.id;
            }
            for (const auto& property : type.properties) {
                if ((property.getter && containsCursor(*property.getter)) ||
                    (property.setter && containsCursor(*property.setter))) {
                    requesterTypeId = type.id;
                }
            }
        }
    }
    const auto derivesFrom = [&](semantic::SymbolId typeId,
                                 semantic::SymbolId baseTypeId) {
        std::unordered_set<semantic::SymbolId> visited;
        while (typeId != 0 && visited.insert(typeId).second) {
            if (typeId == baseTypeId) return true;
            const auto found = types.find(typeId);
            if (found == types.end()) break;
            typeId = found->second->baseTypeId;
        }
        return false;
    };
    const auto accessAllowed = [&](semantic::Accessibility accessibility,
                                   semantic::SymbolId declaringTypeId,
                                   const std::string& moduleName) {
        if (accessibility == semantic::Accessibility::Public) return true;
        if (accessibility == semantic::Accessibility::Internal) {
            return moduleName == requesterModule;
        }
        if (requesterTypeId == 0 || declaringTypeId == 0) return false;
        if (accessibility == semantic::Accessibility::Private) {
            return requesterTypeId == declaringTypeId;
        }
        return derivesFrom(requesterTypeId, declaringTypeId);
    };
    struct VisibilityMeta {
        semantic::Accessibility accessibility = semantic::Accessibility::Public;
        semantic::SymbolId declaringTypeId = 0;
        std::string moduleName;
        bool isType = false;
    };
    std::unordered_map<semantic::SymbolId, VisibilityMeta> visibilityById;
    for (const auto& module : result_.modules) {
        for (const auto& type : module.types) {
            visibilityById[type.id] = {type.accessibility, 0, type.moduleName, true};
            for (const auto& field : type.fields) {
                visibilityById[field.id] = {field.accessibility, field.declaringTypeId, type.moduleName, false};
            }
            for (const auto& method : type.methods) {
                visibilityById[method.id] = {method.accessibility, method.declaringTypeId, method.moduleName, false};
            }
            for (const auto& constructor : type.constructors) {
                visibilityById[constructor.id] = {constructor.accessibility, constructor.declaringTypeId, constructor.moduleName, false};
            }
            for (const auto& property : type.properties) {
                visibilityById[property.id] = {property.accessibility, property.declaringTypeId, type.moduleName, false};
            }
        }
    }
    const auto symbolVisible = [&](semantic::SymbolId id) {
        const auto it = visibilityById.find(id);
        if (it == visibilityById.end()) return true;
        if (it->second.isType) {
            return it->second.accessibility == semantic::Accessibility::Public ||
                it->second.moduleName == requesterModule;
        }
        return accessAllowed(
            it->second.accessibility,
            it->second.declaringTypeId,
            it->second.moduleName);
    };
    const auto* doc = document(path);
    const auto computeByteOffset = [&](text::LinePosition pos) -> std::size_t {
        if (!doc) return 0;
        std::size_t line = 0;
        std::size_t start = 0;
        while (line < pos.line && start < doc->content.size()) {
            const auto next = doc->content.find('\n', start);
            if (next == std::string::npos) return doc->content.size();
            start = next + 1;
            ++line;
        }
        return std::min(start + pos.column, doc->content.size());
    };
    const auto rawCursorOffset = computeByteOffset(position);

    if (doc && rawCursorOffset > 0) {
        std::size_t scan = rawCursorOffset;
        while (scan > 0 &&
               (std::isalnum(static_cast<unsigned char>(doc->content[scan - 1])) ||
                doc->content[scan - 1] == '_')) {
            --scan;
        }
        if (scan > 0 && doc->content[scan - 1] == '.') {
            std::size_t recvEnd = scan - 1;
            const auto isHorizSpace = [](char ch) noexcept {
                return ch == ' ' || ch == '\t';
            };
            while (recvEnd > 0 && isHorizSpace(doc->content[recvEnd - 1])) {
                --recvEnd;
            }
            std::size_t exprStart = recvEnd;
            while (exprStart > 0) {
                while (exprStart > 0 && isHorizSpace(doc->content[exprStart - 1])) {
                    --exprStart;
                }
                if (exprStart > 0 &&
                    (doc->content[exprStart - 1] == ')' || doc->content[exprStart - 1] == ']')) {
                    const char closeCh = doc->content[exprStart - 1];
                    const char openCh = closeCh == ')' ? '(' : '[';
                    int depth = 1;
                    --exprStart;
                    while (exprStart > 0 && depth > 0) {
                        --exprStart;
                        if (doc->content[exprStart] == closeCh) ++depth;
                        else if (doc->content[exprStart] == openCh) --depth;
                    }
                    continue;
                }
                std::size_t idEnd = exprStart;
                while (exprStart > 0 &&
                       (std::isalnum(static_cast<unsigned char>(doc->content[exprStart - 1])) ||
                        doc->content[exprStart - 1] == '_')) {
                    --exprStart;
                }
                if (exprStart == idEnd) break;
                std::size_t beforeId = exprStart;
                while (beforeId > 0 && isHorizSpace(doc->content[beforeId - 1])) {
                    --beforeId;
                }
                if (beforeId > 0 && doc->content[beforeId - 1] == '.') {
                    exprStart = beforeId - 1;
                    continue;
                }
                break;
            }

            const auto trimText = [](std::string text) {
                std::size_t begin = 0;
                while (begin < text.size() &&
                       std::isspace(static_cast<unsigned char>(text[begin]))) {
                    ++begin;
                }
                std::size_t end = text.size();
                while (end > begin &&
                       std::isspace(static_cast<unsigned char>(text[end - 1]))) {
                    --end;
                }
                return text.substr(begin, end - begin);
            };

            const std::string chainText =
                trimText(doc->content.substr(exprStart, recvEnd - exprStart));
            if (!chainText.empty()) {
                std::vector<std::string> segments;
                std::size_t segStart = 0;
                int parenDepth = 0;
                int bracketDepth = 0;
                for (std::size_t idx = 0; idx < chainText.size(); ++idx) {
                    const char ch = chainText[idx];
                    if (ch == '(') ++parenDepth;
                    else if (ch == ')' && parenDepth > 0) --parenDepth;
                    else if (ch == '[') ++bracketDepth;
                    else if (ch == ']' && bracketDepth > 0) --bracketDepth;
                    else if (ch == '.' && parenDepth == 0 && bracketDepth == 0) {
                        segments.push_back(trimText(chainText.substr(segStart, idx - segStart)));
                        segStart = idx + 1;
                    }
                }
                segments.push_back(trimText(chainText.substr(segStart)));

                const auto findTypeByName =
                    [&](const std::string& typeName) -> const semantic::TypeSymbol* {
                    if (typeName.empty()) return nullptr;
                    std::string genericTranslated = typeName;
                    const auto ltPos = typeName.find('<');
                    const auto gtPos = typeName.rfind('>');
                    if (ltPos != std::string::npos && gtPos != std::string::npos && gtPos > ltPos) {
                        std::string baseName = trimText(typeName.substr(0, ltPos));
                        std::string argsPart = typeName.substr(ltPos + 1, gtPos - ltPos - 1);
                        genericTranslated = baseName + "__";
                        for (char ch : argsPart) {
                            if (ch == ',') genericTranslated += "__";
                            else if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
                                genericTranslated.push_back(ch);
                            } else if (!std::isspace(static_cast<unsigned char>(ch))) {
                                genericTranslated.push_back('_');
                            }
                        }
                    }
                    for (const auto& module : result_.modules) {
                        for (const auto& type : module.types) {
                            if (semantic::canonicalTypeName(type) == typeName ||
                                type.name == typeName ||
                                type.name == genericTranslated) {
                                return &type;
                            }
                        }
                    }
                    return nullptr;
                };

                const auto resolveMemberOnType =
                    [&](const semantic::TypeSymbol* owner,
                        const std::string& memberName,
                        bool isCall) -> std::string {
                    std::unordered_set<semantic::SymbolId> visited;
                    const semantic::TypeSymbol* curr = owner;
                    while (curr && visited.insert(curr->id).second) {
                        if (isCall) {
                            for (const auto& method : curr->methods) {
                                if (method.name == memberName) {
                                    return typeDetail(method.returnType, method.returnTypeName);
                                }
                            }
                        } else {
                            for (const auto& field : curr->fields) {
                                if (field.name == memberName) {
                                    return typeDetail(field.type, field.typeName);
                                }
                            }
                            for (const auto& prop : curr->properties) {
                                if (prop.name == memberName) {
                                    return typeDetail(prop.type, prop.typeName);
                                }
                            }
                            for (const auto& method : curr->methods) {
                                if (method.name == memberName) {
                                    return typeDetail(method.returnType, method.returnTypeName);
                                }
                            }
                        }
                        const semantic::SymbolId baseId = curr->baseTypeId != 0
                            ? curr->baseTypeId
                            : (!curr->baseTypeName.empty()
                                   ? semantic::stableTypeId(curr->baseTypeName)
                                   : 0);
                        const auto foundBase = types.find(baseId);
                        curr = foundBase != types.end() ? foundBase->second : nullptr;
                    }
                    return {};
                };

                std::string currentTypeName;
                for (std::size_t segIdx = 0; segIdx < segments.size(); ++segIdx) {
                    std::string seg = segments[segIdx];
                    if (seg.empty()) {
                        currentTypeName.clear();
                        break;
                    }
                    int indexCount = 0;
                    while (!seg.empty() && seg.back() == ']') {
                        int depth = 1;
                        std::size_t pos = seg.size() - 1;
                        while (pos > 0 && depth > 0) {
                            --pos;
                            if (seg[pos] == ']') ++depth;
                            else if (seg[pos] == '[') --depth;
                        }
                        if (depth != 0) break;
                        seg = trimText(seg.substr(0, pos));
                        ++indexCount;
                    }
                    while (seg.size() >= 2 && seg.front() == '(' && seg.back() == ')') {
                        int depth = 0;
                        bool wrapsWhole = true;
                        for (std::size_t k = 0; k < seg.size(); ++k) {
                            if (seg[k] == '(') ++depth;
                            else if (seg[k] == ')') {
                                --depth;
                                if (depth == 0 && k + 1 < seg.size()) {
                                    wrapsWhole = false;
                                    break;
                                }
                            }
                        }
                        if (!wrapsWhole || depth != 0) break;
                        seg = trimText(seg.substr(1, seg.size() - 2));
                    }
                    bool isCall = false;
                    if (!seg.empty() && seg.back() == ')') {
                        int depth = 1;
                        std::size_t pos = seg.size() - 1;
                        while (pos > 0 && depth > 0) {
                            --pos;
                            if (seg[pos] == ')') ++depth;
                            else if (seg[pos] == '(') --depth;
                        }
                        if (depth == 0 && pos > 0) {
                            seg = trimText(seg.substr(0, pos));
                            isCall = true;
                        }
                    }
                    if (seg.size() > 4 && seg.rfind("new ", 0) == 0) {
                        seg = trimText(seg.substr(4));
                        isCall = true;
                    }

                    std::string resolvedSegType;
                    if (segIdx == 0) {
                        if (seg == "this" && requesterTypeId != 0) {
                            const auto found = types.find(requesterTypeId);
                            if (found != types.end()) resolvedSegType = found->second->name;
                        }
                        if (resolvedSegType.empty() && isCall) {
                            if (requesterTypeId != 0) {
                                const auto foundReq = types.find(requesterTypeId);
                                if (foundReq != types.end()) {
                                    resolvedSegType =
                                        resolveMemberOnType(foundReq->second, seg, true);
                                }
                            }
                            if (resolvedSegType.empty()) {
                                for (const auto& module : result_.modules) {
                                    for (const auto& fn : module.functions) {
                                        if (fn.name == seg) {
                                            std::string retName;
                                            if (fn.returnTypeId != 0) {
                                                const auto foundRet = types.find(fn.returnTypeId);
                                                if (foundRet != types.end()) {
                                                    retName = foundRet->second->name;
                                                }
                                            }
                                            resolvedSegType = typeDetail(fn.returnType, retName);
                                            break;
                                        }
                                    }
                                    if (!resolvedSegType.empty()) break;
                                }
                            }
                            if (resolvedSegType.empty()) {
                                if (const auto* ctorType = findTypeByName(seg)) {
                                    resolvedSegType = ctorType->name;
                                }
                            }
                        }
                        if (resolvedSegType.empty() && !isCall) {
                            const semantic::SymbolOccurrence* bestLocal = nullptr;
                            (void)allDefinitionsRef();
                            if (const auto pathOccIt = cachedOccByPath_.find(path); pathOccIt != cachedOccByPath_.end()) {
                                for (const std::size_t occIdx : pathOccIt->second) {
                                    const auto& occ = result_.symbols[occIdx];
                                    if (occ.definition &&
                                    occ.name == seg &&
                                    occ.span.start <= rawCursorOffset &&
                                    (occ.kind == semantic::SymbolKind::Local ||
                                     occ.kind == semantic::SymbolKind::Parameter ||
                                     occ.kind == semantic::SymbolKind::Field ||
                                     occ.kind == semantic::SymbolKind::Property)) {
                                    if (!bestLocal || occ.span.start >= bestLocal->span.start) {
                                        bestLocal = &occ;
                                    }
                                    }
                                }
                            }
                            if (bestLocal && !bestLocal->detail.empty()) {
                                resolvedSegType = bestLocal->detail;
                            }
                        }
                        if (resolvedSegType.empty() && requesterTypeId != 0) {
                            const auto foundReq = types.find(requesterTypeId);
                            if (foundReq != types.end()) {
                                resolvedSegType =
                                    resolveMemberOnType(foundReq->second, seg, isCall);
                            }
                        }
                        if (resolvedSegType.empty()) {
                            if (const auto* namedType = findTypeByName(seg)) {
                                resolvedSegType = namedType->name;
                            }
                        }
                    } else {
                        if (currentTypeName == "string") {
                            if (seg == "Length" || seg == "Count" || seg == "IndexOf") {
                                resolvedSegType = "int";
                            } else if (seg == "IsEmpty" || seg == "Contains" ||
                                       seg == "StartsWith" || seg == "EndsWith") {
                                resolvedSegType = "bool";
                            } else if (seg == "Substring" || seg == "Trim" ||
                                       seg == "Replace" || seg == "ToUpper" ||
                                       seg == "ToLower") {
                                resolvedSegType = "string";
                            }
                        } else if (currentTypeName.size() > 2 &&
                                   currentTypeName.substr(currentTypeName.size() - 2) == "[]") {
                            if (seg == "length" || seg == "Length" || seg == "Count") {
                                resolvedSegType = "int";
                            }
                        } else if (const auto* ownerType = findTypeByName(currentTypeName)) {
                            resolvedSegType = resolveMemberOnType(ownerType, seg, isCall);
                        }
                    }

                    for (int idx = 0; idx < indexCount; ++idx) {
                        if (resolvedSegType.size() > 2 &&
                            resolvedSegType.substr(resolvedSegType.size() - 2) == "[]") {
                            resolvedSegType.resize(resolvedSegType.size() - 2);
                        }
                    }
                    currentTypeName = std::move(resolvedSegType);
                    if (currentTypeName.empty()) break;
                }

                if (currentTypeName == "string") {
                    std::vector<CompletionItem> stringItems = {
                        {"Contains", "bool Contains(string value)", semantic::SymbolKind::Function},
                        {"Count", "int", semantic::SymbolKind::Property},
                        {"EndsWith", "bool EndsWith(string suffix)", semantic::SymbolKind::Function},
                        {"IndexOf", "int IndexOf(string value)", semantic::SymbolKind::Function},
                        {"IsEmpty", "bool", semantic::SymbolKind::Property},
                        {"Length", "int", semantic::SymbolKind::Property},
                        {"Replace", "string Replace(string oldValue, string newValue)", semantic::SymbolKind::Function},
                        {"StartsWith", "bool StartsWith(string prefix)", semantic::SymbolKind::Function},
                        {"Substring", "string Substring(int start, int length)", semantic::SymbolKind::Function},
                        {"ToLower", "string ToLower()", semantic::SymbolKind::Function},
                        {"ToUpper", "string ToUpper()", semantic::SymbolKind::Function},
                        {"Trim", "string Trim()", semantic::SymbolKind::Function},
                    };
                    return stringItems;
                }
                if (currentTypeName.size() > 2 &&
                    currentTypeName.substr(currentTypeName.size() - 2) == "[]") {
                    std::vector<CompletionItem> arrayItems = {
                        {"Count", "int", semantic::SymbolKind::Property},
                        {"Length", "int", semantic::SymbolKind::Property},
                        {"length", "int", semantic::SymbolKind::Property},
                    };
                    return arrayItems;
                }

                const semantic::TypeSymbol* targetType = findTypeByName(currentTypeName);
                if (targetType) {
                    std::vector<CompletionItem> memberItems;
                    std::set<std::pair<std::string, std::string>> seenMembers;
                    std::unordered_set<semantic::SymbolId> visitedTypes;
                    const semantic::TypeSymbol* curr = targetType;
                    while (curr && visitedTypes.insert(curr->id).second) {
                        for (const auto& field : curr->fields) {
                            if (field.synthetic) continue;
                            if (!accessAllowed(
                                    field.accessibility,
                                    field.declaringTypeId != 0 ? field.declaringTypeId : curr->id,
                                    curr->moduleName)) {
                                continue;
                            }
                            const auto detailStr = typeDetail(field.type, field.typeName);
                            if (seenMembers.emplace(field.name, detailStr).second) {
                                memberItems.push_back(
                                    {field.name, detailStr, semantic::SymbolKind::Field});
                            }
                        }
                        for (const auto& prop : curr->properties) {
                            if (!accessAllowed(
                                    prop.accessibility,
                                    prop.declaringTypeId != 0 ? prop.declaringTypeId : curr->id,
                                    curr->moduleName)) {
                                continue;
                            }
                            const auto detailStr = typeDetail(prop.type, prop.typeName);
                            if (seenMembers.emplace(prop.name, detailStr).second) {
                                memberItems.push_back(
                                    {prop.name, detailStr, semantic::SymbolKind::Property});
                            }
                        }
                        for (const auto& method : curr->methods) {
                            if (method.synthetic) continue;
                            if (!accessAllowed(
                                    method.accessibility,
                                    method.declaringTypeId != 0 ? method.declaringTypeId : curr->id,
                                    curr->moduleName)) {
                                continue;
                            }
                            const auto detailStr = functionDetail(method);
                            if (seenMembers.emplace(method.name, detailStr).second) {
                                memberItems.push_back(
                                    {method.name, detailStr, semantic::SymbolKind::Function});
                            }
                        }
                        for (const auto& enumMember : curr->enumMembers) {
                            const auto detailStr = std::to_string(enumMember.value);
                            if (seenMembers.emplace(enumMember.name, detailStr).second) {
                                memberItems.push_back(
                                    {enumMember.name, detailStr, semantic::SymbolKind::EnumMember});
                            }
                        }
                        const semantic::SymbolId baseId = curr->baseTypeId != 0
                            ? curr->baseTypeId
                            : (!curr->baseTypeName.empty()
                                   ? semantic::stableTypeId(curr->baseTypeName)
                                   : 0);
                        const auto foundBase = types.find(baseId);
                        curr = foundBase != types.end() ? foundBase->second : nullptr;
                    }
                    std::sort(
                        memberItems.begin(),
                        memberItems.end(),
                        [](const auto& left, const auto& right) {
                            return left.label < right.label;
                        });
                    return memberItems;
                }
            }
        }
    }

    std::vector<CompletionItem> output;
    const std::vector<std::string> keywords = {
        "module", "import", "class", "struct", "enum", "interface",
        "delegate", "event", "sequence", "yield", "wait_ticks", "wait_seconds", "wait_event", "wait_until",
        "static", "const", "readonly",
        "public", "private", "protected", "internal", "abstract", "virtual",
        "override", "sealed", "base", "return", "if", "else", "while",
        "for", "foreach", "do", "break", "continue", "switch", "case",
        "default", "when", "new", "this", "true", "false", "null", "var",
        "ref", "out", "in", "params", "where", "is", "as", "typeof",
        "checked", "unchecked", "throw", "try", "catch", "finally", "get",
        "set", "bool", "byte", "sbyte", "short", "ushort", "int", "uint",
        "long", "ulong", "float", "double", "char", "string", "handle",
        "void",
    };
    for (const auto& keyword : keywords) output.push_back({keyword, "keyword", semantic::SymbolKind::Module});
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& symbol : allDefinitionsRef()) {
        if (!symbolVisible(symbol.id)) continue;
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

std::optional<SignatureHelp> LanguageService::signatureHelp(
    const std::string& rawPath,
    text::LinePosition position) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    const auto* doc = document(path);
    if (!doc) return std::nullopt;

    std::size_t line = 0;
    std::size_t start = 0;
    while (line < position.line && start < doc->content.size()) {
        const auto next = doc->content.find('\n', start);
        if (next == std::string::npos) {
            start = doc->content.size();
            break;
        }
        start = next + 1;
        ++line;
    }
    const std::size_t cursorOffset = std::min(start + position.column, doc->content.size());
    if (cursorOffset == 0) return std::nullopt;

    int parenDepth = 0;
    std::size_t activeParam = 0;
    std::size_t openParenPos = std::string::npos;
    for (std::size_t idx = cursorOffset; idx > 0; --idx) {
        const char ch = doc->content[idx - 1];
        if (ch == ')') {
            ++parenDepth;
        } else if (ch == '(') {
            if (parenDepth == 0) {
                openParenPos = idx - 1;
                break;
            }
            --parenDepth;
        } else if (ch == ',' && parenDepth == 0) {
            ++activeParam;
        }
    }
    if (openParenPos == std::string::npos) return std::nullopt;

    std::size_t nameEnd = openParenPos;
    while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(doc->content[nameEnd - 1]))) {
        --nameEnd;
    }
    std::size_t nameStart = nameEnd;
    while (nameStart > 0 &&
           (std::isalnum(static_cast<unsigned char>(doc->content[nameStart - 1])) ||
            doc->content[nameStart - 1] == '_')) {
        --nameStart;
    }
    const std::string calleeName = doc->content.substr(nameStart, nameEnd - nameStart);
    if (calleeName.empty()) return std::nullopt;

    SignatureHelp help;
    help.activeParameter = activeParam;
    std::set<std::string> seenLabels;

    auto addFunctionSymbol = [&](const semantic::FunctionSymbol& fn) {
        const std::string label = functionDetail(fn);
        if (!seenLabels.insert(label).second) return;
        SignatureInfo sig;
        sig.label = label;
        sig.documentation = fn.moduleName.empty()
            ? fn.name
            : (fn.ownerTypeName.empty()
                   ? fn.moduleName + "::" + fn.name
                   : fn.moduleName + "::" + fn.ownerTypeName + "." + fn.name);
        const std::size_t paramBegin = fn.method && !fn.staticMethod ? 1 : 0;
        for (std::size_t idx = paramBegin; idx < fn.parameters.size(); ++idx) {
            const auto& param = fn.parameters[idx];
            const auto pType = typeDetail(param.type, param.typeName);
            sig.parameters.push_back({pType + " " + param.name, pType});
        }
        help.signatures.push_back(std::move(sig));
    };

    for (const auto& module : result_.modules) {
        for (const auto& type : module.types) {
            for (const auto& method : type.methods) {
                if (!method.synthetic && method.name == calleeName) {
                    addFunctionSymbol(method);
                }
            }
            if (type.name == calleeName) {
                for (const auto& ctor : type.constructors) {
                    if (!ctor.synthetic) {
                        addFunctionSymbol(ctor);
                    }
                }
            }
        }
        for (const auto& fn : module.functions) {
            if (fn.name == calleeName) {
                semantic::FunctionSymbol sym;
                sym.moduleName = module.name;
                sym.name = fn.name;
                sym.returnType = fn.returnType;
                sym.parameters.reserve(fn.parameterTypes.size());
                for (std::size_t idx = 0; idx < fn.parameterTypes.size(); ++idx) {
                    semantic::VariableSymbol param;
                    param.name = idx < fn.debugInfo.locals.size()
                        ? fn.debugInfo.locals[idx].name
                        : ("arg" + std::to_string(idx));
                    param.type = fn.parameterTypes[idx];
                    sym.parameters.push_back(std::move(param));
                }
                addFunctionSymbol(sym);
            }
        }
    }

    if (help.signatures.empty()) return std::nullopt;
    for (std::size_t idx = 0; idx < help.signatures.size(); ++idx) {
        if (help.signatures[idx].parameters.size() > activeParam) {
            help.activeSignature = idx;
            break;
        }
    }
    return help;
}

std::optional<HoverInfo> LanguageService::hover(
    const std::string& rawPath,
    text::LinePosition position) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    const auto symbol = resolve(path, position);
    if (!symbol) return std::nullopt;
    return HoverInfo{symbolKindName(symbol->kind) + std::string{" "} +
        symbol->name + (symbol->detail.empty() ? "" : ": " + symbol->detail),
        symbol->location};
}

std::optional<Location> LanguageService::definition(
    const std::string& rawPath,
    text::LinePosition position) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    const auto symbol = resolve(path, position);
    return symbol ? std::optional<Location>(symbol->location) : std::nullopt;
}

std::vector<Location> LanguageService::references(
    const std::string& rawPath,
    text::LinePosition position,
    bool includeDefinition) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    const auto token = tokenAt(path, position);
    const auto symbol = resolve(path, position);
    if (!token || !symbol) return {};
    std::vector<Location> output;

    if (symbol->id != 0) {
        bool hasAnySymbolOccurrence = false;
        std::set<std::tuple<std::string, std::size_t, std::size_t>> seenSpans;
        for (const auto& occ : result_.symbols) {
            if (occ.id != symbol->id) continue;
            if (document(occ.sourceName) == nullptr) {
                continue;
            }
            hasAnySymbolOccurrence = true;
            const bool definitionLocation =
                occ.definition ||
                (occ.sourceName == symbol->location.path &&
                 occ.span.start == symbol->location.range.span.start);
            if (!includeDefinition && definitionLocation) continue;
            if (seenSpans.emplace(occ.sourceName, occ.span.start, occ.span.length).second) {
                output.push_back({occ.sourceName, rangeFor(occ.sourceName, occ.span)});
            }
        }
        if (includeDefinition && document(symbol->location.path) != nullptr) {
            const auto defSpan = symbol->location.range.span;
            if (seenSpans.emplace(symbol->location.path, defSpan.start, defSpan.length).second) {
                output.push_back(symbol->location);
            }
        }
        if (hasAnySymbolOccurrence || symbol->kind != semantic::SymbolKind::Type) {
            std::sort(output.begin(), output.end(), [](const auto& left, const auto& right) {
                if (left.path != right.path) return left.path < right.path;
                return left.range.span.start < right.range.span.start;
            });
            return output;
        }
    }

    for (const auto& documentPath : documentPaths()) {
        const auto* docPtr = document(documentPath);
        if (!docPtr) continue;
        text::SourceText source(docPtr->content, documentPath);
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

std::vector<DocumentSymbol> LanguageService::documentSymbols(const std::string& rawPath) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    std::vector<DocumentSymbol> output;
    for (const auto& symbol : allDefinitionsRef()) {
        if (symbol.location.path == path &&
            symbol.kind != semantic::SymbolKind::Local &&
            symbol.kind != semantic::SymbolKind::Parameter) {
            output.push_back(symbol);
        }
    }
    return output;
}

std::vector<SemanticToken> LanguageService::semanticTokens(const std::string& rawPath) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    const auto* doc = document(path);
    if (!doc) return {};

    // Legend: 0=namespace, 1=type, 2=function, 3=variable, 4=parameter, 5=property, 6=enumMember, 7=keyword, 8=number, 9=string
    const auto mapKindToTokenType = [](semantic::SymbolKind kind) -> std::uint32_t {
        switch (kind) {
        case semantic::SymbolKind::Module: return 0;
        case semantic::SymbolKind::Type: return 1;
        case semantic::SymbolKind::Function: return 2;
        case semantic::SymbolKind::Local: return 3;
        case semantic::SymbolKind::Parameter: return 4;
        case semantic::SymbolKind::Field:
        case semantic::SymbolKind::Property: return 5;
        case semantic::SymbolKind::EnumMember: return 6;
        }
        return 3;
    };

    (void)allDefinitionsRef();
    std::unordered_map<std::size_t, const semantic::SymbolOccurrence*> occByStart;
    if (const auto pathOccIt = cachedOccByPath_.find(path); pathOccIt != cachedOccByPath_.end()) {
        occByStart.reserve(pathOccIt->second.size());
        for (const std::size_t occIdx : pathOccIt->second) {
            const auto& occ = result_.symbols[occIdx];
            if (occ.span.length > 0) {
                occByStart[occ.span.start] = &occ;
            }
        }
    }

    std::vector<SemanticToken> tokens;
    for (const auto& tok : documentTokens(path, *doc)) {
        if (tok.kind == syntax::SyntaxKind::EndOfFileToken || tok.span.length == 0) continue;
        const auto rng = rangeFor(path, tok.span);
        const std::size_t line0 = rng.start.line > 0 ? rng.start.line - 1 : 0;
        const std::size_t col0 = rng.start.column > 0 ? rng.start.column - 1 : 0;

        if (const auto it = occByStart.find(tok.span.start); it != occByStart.end()) {
            const std::uint32_t mods = it->second->definition ? 1u : 0u;
            tokens.push_back(SemanticToken{
                line0, col0, tok.span.length, mapKindToTokenType(it->second->kind), mods});
            continue;
        }
        if (syntax::isPrimitiveTypeKeyword(tok.kind)) {
            tokens.push_back(SemanticToken{line0, col0, tok.span.length, 1u, 0u});
        } else if (tok.kind == syntax::SyntaxKind::IntegerLiteralToken ||
                   tok.kind == syntax::SyntaxKind::FloatLiteralToken) {
            tokens.push_back(SemanticToken{line0, col0, tok.span.length, 8u, 0u});
        } else if (tok.kind == syntax::SyntaxKind::StringLiteralToken ||
                   tok.kind == syntax::SyntaxKind::InterpolatedStringToken) {
            tokens.push_back(SemanticToken{line0, col0, tok.span.length, 9u, 0u});
        } else if (tok.kind != syntax::SyntaxKind::IdentifierToken &&
                   !tok.text.empty() &&
                   std::isalpha(static_cast<unsigned char>(tok.text.front()))) {
            tokens.push_back(SemanticToken{line0, col0, tok.span.length, 7u, 0u});
        }
    }
    return tokens;
}

std::vector<std::uint32_t> LanguageService::semanticTokensEncoded(const std::string& path) {
    const auto raw = semanticTokens(path);
    std::vector<std::uint32_t> data;
    data.reserve(raw.size() * 5);
    std::size_t prevLine = 0;
    std::size_t prevChar = 0;
    for (const auto& t : raw) {
        const std::size_t deltaLine = t.line - prevLine;
        const std::size_t deltaStart = (deltaLine == 0) ? (t.startChar - prevChar) : t.startChar;
        data.push_back(static_cast<std::uint32_t>(deltaLine));
        data.push_back(static_cast<std::uint32_t>(deltaStart));
        data.push_back(static_cast<std::uint32_t>(t.length));
        data.push_back(t.tokenType);
        data.push_back(t.tokenModifiers);
        prevLine = t.line;
        prevChar = t.startChar;
    }
    return data;
}

std::vector<InlayHint> LanguageService::inlayHints(const std::string& rawPath) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    const auto* doc = document(path);
    if (!doc) return {};

    (void)allDefinitionsRef();
    std::vector<InlayHint> hints;
    const auto& allTokens = documentTokens(path, *doc);
    const auto pathOccIt = cachedOccByPath_.find(path);

    // 1. Type inlay hints for `var name = ...` declarations
    for (std::size_t i = 0; i + 1 < allTokens.size(); ++i) {
        if (allTokens[i].text == "var" &&
            allTokens[i + 1].kind == syntax::SyntaxKind::IdentifierToken) {
            const auto& idTok = allTokens[i + 1];
            if (pathOccIt != cachedOccByPath_.end()) {
                for (const std::size_t occIdx : pathOccIt->second) {
                    const auto& occ = result_.symbols[occIdx];
                    if (occ.definition && occ.span.start == idTok.span.start && !occ.detail.empty()) {
                        const auto rng = rangeFor(path, idTok.span);
                        const std::size_t line0 = rng.end.line > 0 ? rng.end.line - 1 : 0;
                        const std::size_t col0 = rng.end.column > 0 ? rng.end.column - 1 : 0;
                        hints.push_back(InlayHint{{line0, col0}, ": " + occ.detail, 1u});
                        break;
                    }
                }
            }
        }
    }

    // 2. Call-site parameter name inlay hints (`paramName:`) with arity-aware overload resolution
    std::unordered_set<std::string> calledIdentNames;
    for (std::size_t i = 0; i + 2 < allTokens.size(); ++i) {
        if (allTokens[i].kind == syntax::SyntaxKind::IdentifierToken &&
            allTokens[i + 1].kind == syntax::SyntaxKind::OpenParenToken &&
            allTokens[i + 2].kind != syntax::SyntaxKind::CloseParenToken) {
            calledIdentNames.insert(allTokens[i].text);
        }
    }
    if (calledIdentNames.empty()) return hints;

    std::unordered_map<std::string, std::vector<std::vector<std::string>>> paramNamesByFunction;
    const auto addOverloadParams = [&](const std::string& name, std::vector<std::string> names) {
        if (names.empty()) return;
        auto& overloads = paramNamesByFunction[name];
        if (std::find(overloads.begin(), overloads.end(), names) == overloads.end()) {
            overloads.push_back(std::move(names));
        }
    };
    for (const auto& module : result_.modules) {
        for (const auto& fn : module.functions) {
            if (calledIdentNames.find(fn.name) == calledIdentNames.end()) continue;
            std::vector<std::string> names;
            for (std::size_t idx = 0; idx < fn.parameterTypes.size(); ++idx) {
                if (idx < fn.debugInfo.locals.size() && !fn.debugInfo.locals[idx].name.empty()) {
                    names.push_back(fn.debugInfo.locals[idx].name);
                } else {
                    names.push_back("arg" + std::to_string(idx));
                }
            }
            addOverloadParams(fn.name, std::move(names));
        }
        for (const auto& type : module.types) {
            for (const auto& method : type.methods) {
                if (method.synthetic ||
                    calledIdentNames.find(method.name) == calledIdentNames.end()) {
                    continue;
                }
                std::vector<std::string> names;
                const std::size_t beginIdx = (method.method && !method.staticMethod) ? 1 : 0;
                for (std::size_t idx = beginIdx; idx < method.parameters.size(); ++idx) {
                    names.push_back(method.parameters[idx].name);
                }
                addOverloadParams(method.name, std::move(names));
            }
        }
    }

    for (std::size_t i = 0; i + 2 < allTokens.size(); ++i) {
        if (allTokens[i].kind != syntax::SyntaxKind::IdentifierToken ||
            allTokens[i + 1].kind != syntax::SyntaxKind::OpenParenToken) {
            continue;
        }
        // Skip function/method declarations
        if (i > 0) {
            const auto prevKind = allTokens[i - 1].kind;
            if (prevKind == syntax::SyntaxKind::IdentifierToken ||
                syntax::isPrimitiveTypeKeyword(prevKind) ||
                prevKind == syntax::SyntaxKind::VoidKeyword ||
                prevKind == syntax::SyntaxKind::SequenceKeyword ||
                prevKind == syntax::SyntaxKind::NewKeyword) {
                continue;
            }
        }
        const auto fnIt = paramNamesByFunction.find(allTokens[i].text);
        if (fnIt == paramNamesByFunction.end() || fnIt->second.empty()) continue;

        std::size_t j = i + 2;
        if (j < allTokens.size() && allTokens[j].kind == syntax::SyntaxKind::CloseParenToken) {
            continue;
        }
        // Count call-site arguments first to match overload arity
        std::size_t argCount = 1;
        {
            std::size_t scan = j;
            int scanDepth = 0;
            while (scan < allTokens.size()) {
                const auto sk = allTokens[scan].kind;
                if (sk == syntax::SyntaxKind::EndOfFileToken) break;
                if (scanDepth == 0 && sk == syntax::SyntaxKind::CloseParenToken) break;
                if (sk == syntax::SyntaxKind::OpenParenToken ||
                    sk == syntax::SyntaxKind::OpenBracketToken ||
                    sk == syntax::SyntaxKind::OpenBraceToken) {
                    ++scanDepth;
                } else if (sk == syntax::SyntaxKind::CloseParenToken ||
                           sk == syntax::SyntaxKind::CloseBracketToken ||
                           sk == syntax::SyntaxKind::CloseBraceToken) {
                    --scanDepth;
                } else if (sk == syntax::SyntaxKind::CommaToken && scanDepth == 0) {
                    ++argCount;
                }
                ++scan;
            }
        }
        const std::vector<std::string>* selectedParams = nullptr;
        for (const auto& candidate : fnIt->second) {
            if (candidate.size() == argCount) {
                selectedParams = &candidate;
                break;
            }
        }
        if (!selectedParams) {
            for (const auto& candidate : fnIt->second) {
                if (candidate.size() >= argCount) {
                    selectedParams = &candidate;
                    break;
                }
            }
        }
        if (!selectedParams) continue;
        const auto& pNames = *selectedParams;

        int depth = 0;
        std::size_t paramIdx = 0;
        bool atArgStart = true;
        while (j < allTokens.size()) {
            const auto k = allTokens[j].kind;
            if (k == syntax::SyntaxKind::EndOfFileToken) break;
            if (depth == 0 && k == syntax::SyntaxKind::CloseParenToken) break;
            if (atArgStart && depth == 0 && paramIdx < pNames.size()) {
                const bool isNamedArg =
                    (j + 1 < allTokens.size() &&
                     allTokens[j + 1].kind == syntax::SyntaxKind::ColonToken);
                if (!isNamedArg && allTokens[j].text != pNames[paramIdx]) {
                    const auto rng = rangeFor(path, allTokens[j].span);
                    const std::size_t line0 = rng.start.line > 0 ? rng.start.line - 1 : 0;
                    const std::size_t col0 = rng.start.column > 0 ? rng.start.column - 1 : 0;
                    hints.push_back(InlayHint{{line0, col0}, pNames[paramIdx] + ":", 2u});
                }
                atArgStart = false;
            }
            if (k == syntax::SyntaxKind::OpenParenToken ||
                k == syntax::SyntaxKind::OpenBracketToken ||
                k == syntax::SyntaxKind::OpenBraceToken) {
                ++depth;
            } else if (k == syntax::SyntaxKind::CloseParenToken ||
                       k == syntax::SyntaxKind::CloseBracketToken ||
                       k == syntax::SyntaxKind::CloseBraceToken) {
                --depth;
            } else if (k == syntax::SyntaxKind::CommaToken && depth == 0) {
                ++paramIdx;
                atArgStart = true;
            }
            ++j;
        }
    }

    return hints;
}

std::vector<TextEdit> LanguageService::formatDocument(const std::string& rawPath, std::size_t tabSize) {
    const std::string path = normalizeDocPath(rawPath);
    const auto* doc = document(path);
    if (!doc) return {};
    std::string formatted;
    formatted.reserve(doc->content.size() + 64);

    int indentLevel = 0;
    std::size_t pos = 0;
    while (pos < doc->content.size()) {
        std::size_t lineEnd = doc->content.find('\n', pos);
        const bool hasNewline = lineEnd != std::string::npos;
        if (!hasNewline) lineEnd = doc->content.size();

        std::string line = doc->content.substr(pos, lineEnd - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::size_t firstNonSpace = 0;
        while (firstNonSpace < line.size() &&
               (line[firstNonSpace] == ' ' || line[firstNonSpace] == '\t')) {
            ++firstNonSpace;
        }
        std::size_t lastNonSpace = line.size();
        while (lastNonSpace > firstNonSpace &&
               (line[lastNonSpace - 1] == ' ' || line[lastNonSpace - 1] == '\t')) {
            --lastNonSpace;
        }
        const std::string trimmed = line.substr(firstNonSpace, lastNonSpace - firstNonSpace);

        if (!trimmed.empty()) {
            int leadingClose = 0;
            for (char ch : trimmed) {
                if (ch == '}') ++leadingClose;
                else break;
            }
            const int effectiveIndent = std::max(0, indentLevel - leadingClose);
            formatted.append(static_cast<std::size_t>(effectiveIndent) * tabSize, ' ');
            formatted.append(trimmed);

            int netBraces = 0;
            bool inStr = false;
            for (std::size_t i = 0; i < trimmed.size(); ++i) {
                if (trimmed[i] == '"') {
                    std::size_t bsCount = 0;
                    std::size_t back = i;
                    while (back > 0 && trimmed[back - 1] == '\\') {
                        ++bsCount;
                        --back;
                    }
                    if ((bsCount % 2) == 0) {
                        inStr = !inStr;
                    }
                } else if (!inStr) {
                    if (trimmed[i] == '/' && i + 1 < trimmed.size() && trimmed[i + 1] == '/') {
                        break;
                    }
                    if (trimmed[i] == '{') ++netBraces;
                    else if (trimmed[i] == '}') --netBraces;
                }
            }
            indentLevel = std::max(0, indentLevel + netBraces);
        }
        if (hasNewline) formatted.push_back('\n');
        pos = lineEnd + 1;
    }

    if (formatted == doc->content) return {};
    const auto fullRange = rangeFor(path, text::TextSpan{0, doc->content.size()});
    return {TextEdit{Location{path, fullRange}, std::move(formatted)}};
}

std::vector<DocumentSymbol> LanguageService::workspaceSymbols(const std::string& query) {
    (void)build();
    std::string lowerQuery = query;
    for (char& ch : lowerQuery) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    std::vector<DocumentSymbol> output;
    for (const auto& symbol : allDefinitionsRef()) {
        if (symbol.kind == semantic::SymbolKind::Local ||
            symbol.kind == semantic::SymbolKind::Parameter) {
            continue;
        }
        if (!lowerQuery.empty()) {
            std::string lowerName = symbol.name;
            for (char& ch : lowerName) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            }
            if (lowerName.find(lowerQuery) == std::string::npos) continue;
        }
        output.push_back(symbol);
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
