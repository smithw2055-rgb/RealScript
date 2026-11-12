#include "realscript/tooling/LanguageService.h"

#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"
#include "realscript/game/GameProductization.h"
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

bool hasSceneOrPrefabExtension(const std::filesystem::path& p) {
    const std::string filename = p.filename().string();
    return endsWithIgnoreCase(filename, ".scene.txt") ||
        endsWithIgnoreCase(filename, ".prefab.txt") ||
        endsWithIgnoreCase(filename, ".guid.txt");
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

    if (const auto* sceneEnts = parsed->find("sceneEntities");
        sceneEnts && sceneEnts->isArray()) {
        if (!merge) configManifest_.entities.clear();
        for (const auto& item : sceneEnts->arrayValue()) {
            if (item.isString() && !item.stringValue().empty()) {
                SceneEntityInfo info;
                info.name = item.stringValue();
                info.nodePath = "/" + info.name;
                configManifest_.entities.push_back(std::move(info));
                continue;
            }
            if (!item.isObject()) continue;
            SceneEntityInfo info;
            if (const auto* n = item.find("name"); n && n->isString()) info.name = n->stringValue();
            if (const auto* np = item.find("nodePath"); np && np->isString()) info.nodePath = np->stringValue();
            else if (const auto* p = item.find("path"); p && p->isString()) info.nodePath = p->stringValue();
            if (const auto* idVal = item.find("id"); idVal && idVal->isNumber()) {
                info.entityId = static_cast<std::uint64_t>(std::max<std::int64_t>(0, idVal->integerValue()));
            }
            if (const auto* g = item.find("guid"); g && g->isString()) info.guid = g->stringValue();
            if (const auto* comps = item.find("components"); comps && comps->isArray()) {
                for (const auto& c : comps->arrayValue()) {
                    if (c.isString() && !c.stringValue().empty()) info.components.push_back(c.stringValue());
                }
            }
            if (!info.name.empty() || !info.nodePath.empty()) {
                if (info.name.empty()) info.name = info.nodePath;
                if (info.nodePath.empty()) info.nodePath = "/" + info.name;
                configManifest_.entities.push_back(std::move(info));
            }
        }
        manifestDirty_ = true;
        loadedAnySection = true;
    }

    if (const auto* sigArr = parsed->find("signals");
        sigArr && sigArr->isArray()) {
        if (!merge) configManifest_.signals.clear();
        for (const auto& item : sigArr->arrayValue()) {
            if (item.isString() && !item.stringValue().empty()) {
                SignalInfo s;
                s.name = item.stringValue();
                configManifest_.signals.push_back(std::move(s));
                continue;
            }
            if (!item.isObject()) continue;
            SignalInfo s;
            if (const auto* n = item.find("name"); n && n->isString()) s.name = n->stringValue();
            if (const auto* o = item.find("ownerType"); o && o->isString()) s.ownerType = o->stringValue();
            if (const auto* sig = item.find("signature"); sig && sig->isString()) s.signature = sig->stringValue();
            if (!s.name.empty()) configManifest_.signals.push_back(std::move(s));
        }
        manifestDirty_ = true;
        loadedAnySection = true;
    }

    if (const auto* resArr = parsed->find("resources");
        resArr && resArr->isArray()) {
        if (!merge) configManifest_.resources.clear();
        for (const auto& item : resArr->arrayValue()) {
            if (item.isString() && !item.stringValue().empty()) {
                AssetResourceInfo r;
                r.path = item.stringValue();
                r.guid = game::AssetGuid::fromAssetPath(r.path).toString();
                configManifest_.resources.push_back(std::move(r));
                continue;
            }
            if (!item.isObject()) continue;
            AssetResourceInfo r;
            if (const auto* p = item.find("path"); p && p->isString()) r.path = p->stringValue();
            if (const auto* g = item.find("guid"); g && g->isString()) r.guid = g->stringValue();
            if (const auto* t = item.find("resourceType"); t && t->isString()) r.resourceType = t->stringValue();
            else if (const auto* ty = item.find("type"); ty && ty->isString()) r.resourceType = ty->stringValue();
            if (!r.path.empty()) {
                if (r.guid.empty()) r.guid = game::AssetGuid::fromAssetPath(r.path).toString();
                configManifest_.resources.push_back(std::move(r));
            }
        }
        manifestDirty_ = true;
        loadedAnySection = true;
    }

    if (parseLintConfigFromProjectJson(*parsed, lintConfig_, merge)) {
        loadedAnySection = true;
    }

    if (!loadedAnySection) return false;
    structuralDirty_ = true;
    dirty_ = true;
    manifestDirty_ = true;
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

void LanguageService::setLintConfig(LintConfig config) {
    lintConfig_ = std::move(config);
    hasCachedDiagnostics_ = false;
}

std::vector<LanguageDiagnostic> LanguageService::lint(const std::string& rawPath) {
    return lint(rawPath, lintConfig_);
}

std::vector<LanguageDiagnostic> LanguageService::lint(
    const std::string& rawPath,
    const LintConfig& overrideConfig) {
    const std::string path = normalizeDocPath(rawPath);
    if (!contains(path)) return {};
    (void)build();
    const auto* doc = document(path);
    if (!doc) return {};

    std::vector<compiler::SourceFile> wsSources;
    const auto allPaths = documentPaths();
    wsSources.reserve(allPaths.size() + ambientDocuments_.size());
    for (const auto& p : allPaths) {
        if (const auto* d = document(p)) {
            wsSources.push_back({p, d->content});
        }
    }
    for (const auto& [ambPath, ambDoc] : ambientDocuments_) {
        if (documents_.find(ambPath) == documents_.end() &&
            workspaceDocuments_.find(ambPath) == workspaceDocuments_.end()) {
            wsSources.push_back({ambPath, ambDoc.content});
        }
    }

    GovernanceLinter linter(overrideConfig);
    const auto rawDiags = linter.lintSource(path, doc->content, &result_, &wsSources);
    std::vector<LanguageDiagnostic> output;
    output.reserve(rawDiags.size());
    for (const auto& item : rawDiags) {
        const auto mappedSeverity = (item.severity == LintSeverity::Error)
            ? diagnostics::DiagnosticSeverity::Error
            : diagnostics::DiagnosticSeverity::Warning;
        output.push_back(LanguageDiagnostic{
            item.code,
            item.message,
            mappedSeverity,
            Location{path, rangeFor(path, item.span)},
            item.severity,
        });
    }
    return output;
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
            } else if (hasSceneOrPrefabExtension(p)) {
                std::string sceneText;
                if (readFileContents(p, sceneText)) {
                    sceneDiskFiles_[normalizeDocPath(p.generic_string())] = std::move(sceneText);
                    manifestDirty_ = true;
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
    for (auto it = sceneDiskFiles_.begin(); it != sceneDiskFiles_.end();) {
        if (!isPathWithinRoot(it->first, rootPath)) {
            ++it;
            continue;
        }
        bool covered = false;
        for (const auto& otherRoot : workspaceRoots_) {
            if (isPathWithinRoot(it->first, otherRoot)) {
                covered = true;
                break;
            }
        }
        if (!covered) it = sceneDiskFiles_.erase(it);
        else ++it;
    }
    manifestDirty_ = true;
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
    if (hasSceneOrPrefabExtension(fsPath)) {
        std::string sceneText;
        if (!readFileContents(fsPath, sceneText)) return false;
        sceneDiskFiles_[path] = std::move(sceneText);
        manifestDirty_ = true;
        return true;
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
    if (sceneDiskFiles_.erase(path) > 0) {
        manifestDirty_ = true;
        return true;
    }
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
    manifestDirty_ = true;
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
    manifestDirty_ = true;
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
                        {item.code, item.message, item.severity, {itemPath, rangeFor(itemPath, item.span)}, std::nullopt});
                } else {
                    bool validSpan = false;
                    const std::string ownerPath = inferOwnerForSourceless(item, validSpan);
                    if (!ownerPath.empty()) {
                        cachedDiagnosticsByPath_[ownerPath].push_back({
                            item.code,
                            item.message,
                            item.severity,
                            {ownerPath, validSpan ? rangeFor(ownerPath, item.span) : debug::SourceRange{}},
                            std::nullopt,
                        });
                    }
                }
            }
        }
        hasCachedDiagnostics_ = true;
    }
    const auto it = cachedDiagnosticsByPath_.find(path);
    std::vector<LanguageDiagnostic> out =
        (it != cachedDiagnosticsByPath_.end()) ? it->second : std::vector<LanguageDiagnostic>{};
    if (lintConfig_.enableGovernanceLint) {
        auto govDiags = lint(path, lintConfig_);
        out.insert(
            out.end(),
            std::make_move_iterator(govDiags.begin()),
            std::make_move_iterator(govDiags.end()));
    }
    return out;
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

    // P1-B: String-literal Scene Entity / Signal / Asset Resource completion inside engine calls
    if (doc && rawCursorOffset > 0) {
        // Check if cursor is inside a string literal argument of FindEntity / GetNode / EmitSignal / ConnectSignal / wait_signal / Load
        std::size_t lineStart = rawCursorOffset;
        while (lineStart > 0 && doc->content[lineStart - 1] != '\n') {
            --lineStart;
        }
        int quoteCountBefore = 0;
        std::size_t openQuotePos = std::string::npos;
        for (std::size_t qi = lineStart; qi < rawCursorOffset; ++qi) {
            if (doc->content[qi] == '"' && (qi == lineStart || doc->content[qi - 1] != '\\')) {
                ++quoteCountBefore;
                openQuotePos = qi;
            }
        }
        if ((quoteCountBefore % 2) == 1 && openQuotePos != std::string::npos) {
            const std::string typedPrefix = doc->content.substr(openQuotePos + 1, rawCursorOffset - (openQuotePos + 1));
            std::size_t scanParen = openQuotePos;
            int pDepth = 0;
            std::size_t callOpenParen = std::string::npos;
            std::size_t commaCount = 0;
            while (scanParen > lineStart) {
                --scanParen;
                const char c = doc->content[scanParen];
                if (c == ')') ++pDepth;
                else if (c == '(') {
                    if (pDepth == 0) { callOpenParen = scanParen; break; }
                    --pDepth;
                } else if (c == ',' && pDepth == 0) {
                    ++commaCount;
                }
            }
            if (callOpenParen != std::string::npos) {
                std::size_t fnEnd = callOpenParen;
                while (fnEnd > lineStart && std::isspace(static_cast<unsigned char>(doc->content[fnEnd - 1]))) --fnEnd;
                std::size_t fnStart = fnEnd;
                while (fnStart > lineStart &&
                       (std::isalnum(static_cast<unsigned char>(doc->content[fnStart - 1])) ||
                        doc->content[fnStart - 1] == '_' || doc->content[fnStart - 1] == '.')) {
                    --fnStart;
                }
                const std::string callExpr = doc->content.substr(fnStart, fnEnd - fnStart);
                const auto lastDot = callExpr.rfind('.');
                const std::string shortCallee = (lastDot == std::string::npos) ? callExpr : callExpr.substr(lastDot + 1);

                rebuildSceneManifestIfNeeded();
                std::vector<CompletionItem> manifestItems;
                std::set<std::string> seenLabels;

                const bool isEntityCall =
                    (shortCallee == "FindEntity" || shortCallee == "GetNode" ||
                     shortCallee == "GetEntity" || shortCallee == "FindNode") && commaCount == 0;
                const bool isSignalCall =
                    ((shortCallee == "EmitSignal" || shortCallee == "wait_signal" || shortCallee == "HasSignal") && commaCount == 0) ||
                    (shortCallee == "ConnectSignal" && (commaCount == 0 || commaCount == 1));
                const bool isResourceCall =
                    (shortCallee == "Load" || shortCallee == "LoadAsync" || shortCallee == "Preload" ||
                     callExpr == "ResourceServer.Load" || callExpr == "ResourceServer.LoadAsync") && commaCount == 0;

                if (isEntityCall) {
                    for (const auto& ent : mergedManifest_.entities) {
                        std::string detail = "Entity";
                        if (ent.entityId != 0) detail += " #" + std::to_string(ent.entityId);
                        if (!ent.components.empty()) {
                            detail += " [";
                            for (std::size_t ci = 0; ci < ent.components.size(); ++ci) {
                                if (ci > 0) detail += ", ";
                                detail += ent.components[ci];
                            }
                            detail += "]";
                        }
                        if (!ent.name.empty() && ent.name.find(typedPrefix) == 0 &&
                            seenLabels.insert(ent.name).second) {
                            manifestItems.push_back({ent.name, detail, semantic::SymbolKind::Field});
                        }
                        if (!ent.nodePath.empty() && ent.nodePath != ent.name &&
                            ent.nodePath.find(typedPrefix) == 0 &&
                            seenLabels.insert(ent.nodePath).second) {
                            manifestItems.push_back({ent.nodePath, detail, semantic::SymbolKind::Field});
                        }
                    }
                    return manifestItems;
                }
                if (isSignalCall) {
                    for (const auto& sig : mergedManifest_.signals) {
                        if (sig.name.find(typedPrefix) != 0) continue;
                        if (!seenLabels.insert(sig.name).second) continue;
                        std::string detail = "signal";
                        if (!sig.ownerType.empty()) detail += " on " + sig.ownerType;
                        if (!sig.signature.empty()) detail += " " + sig.signature;
                        manifestItems.push_back({sig.name, detail, semantic::SymbolKind::Property});
                    }
                    return manifestItems;
                }
                if (isResourceCall) {
                    for (const auto& res : mergedManifest_.resources) {
                        if (res.path.find(typedPrefix) == 0 && seenLabels.insert(res.path).second) {
                            std::string detail = res.resourceType.empty() ? "Resource" : res.resourceType;
                            if (!res.guid.empty()) detail += " (guid: " + res.guid + ")";
                            manifestItems.push_back({res.path, detail, semantic::SymbolKind::Module});
                        }
                        if (!res.guid.empty()) {
                            const std::string guidUri = "guid://" + res.guid;
                            if (guidUri.find(typedPrefix) == 0 && seenLabels.insert(guidUri).second) {
                                manifestItems.push_back({guidUri, "AssetGuid -> " + res.path, semantic::SymbolKind::Module});
                            }
                        }
                    }
                    return manifestItems;
                }
            }
        }
    }

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
    if (const auto tok = tokenAt(path, position);
        tok && tok->kind == syntax::SyntaxKind::StringLiteralToken) {
        std::string unquoted = tok->text;
        if (unquoted.size() >= 2 && unquoted.front() == '"' && unquoted.back() == '"') {
            unquoted = unquoted.substr(1, unquoted.size() - 2);
        }
        rebuildSceneManifestIfNeeded();
        const Location tokLoc{path, rangeFor(path, tok->span)};
        for (const auto& ent : mergedManifest_.entities) {
            if (ent.name == unquoted || ent.nodePath == unquoted || (!ent.guid.empty() && ent.guid == unquoted)) {
                std::string info = "scene entity " + ent.name;
                if (!ent.nodePath.empty()) info += " (" + ent.nodePath + ")";
                if (ent.entityId != 0) info += " [id=" + std::to_string(ent.entityId) + "]";
                if (!ent.guid.empty()) info += " guid=" + ent.guid;
                if (!ent.components.empty()) {
                    info += " components: ";
                    for (std::size_t i = 0; i < ent.components.size(); ++i) {
                        if (i > 0) info += ", ";
                        info += ent.components[i];
                    }
                }
                return HoverInfo{std::move(info), tokLoc};
            }
        }
        for (const auto& sig : mergedManifest_.signals) {
            if (sig.name == unquoted) {
                std::string info = "signal " + sig.name;
                if (!sig.ownerType.empty()) info += " (owner: " + sig.ownerType + ")";
                if (!sig.signature.empty()) info += " " + sig.signature;
                return HoverInfo{std::move(info), tokLoc};
            }
        }
        std::string guidLookup = unquoted;
        if (guidLookup.rfind("guid://", 0) == 0) guidLookup = guidLookup.substr(7);
        for (const auto& res : mergedManifest_.resources) {
            if (res.path == unquoted || (!res.guid.empty() && res.guid == guidLookup)) {
                std::string info = "resource " + res.path;
                if (!res.resourceType.empty()) info += " [" + res.resourceType + "]";
                if (!res.guid.empty()) info += " guid=" + res.guid;
                return HoverInfo{std::move(info), tokLoc};
            }
        }
        for (const auto& [typeName, scriptGuid] : mergedManifest_.scriptGuidsByType) {
            if (scriptGuid == guidLookup) {
                return HoverInfo{"script component " + typeName + " guid=" + scriptGuid, tokLoc};
            }
        }
    }
    const auto symbol = resolve(path, position);
    if (!symbol) return std::nullopt;
    std::string hoverText = symbolKindName(symbol->kind) + std::string{" "} +
        symbol->name + (symbol->detail.empty() ? "" : ": " + symbol->detail);
    if (symbol->kind == semantic::SymbolKind::Type) {
        rebuildSceneManifestIfNeeded();
        if (const auto gIt = mergedManifest_.scriptGuidsByType.find(symbol->detail);
            gIt != mergedManifest_.scriptGuidsByType.end()) {
            hoverText += " [Guid=" + gIt->second + "]";
        } else if (const auto gIt2 = mergedManifest_.scriptGuidsByType.find(symbol->name);
                   gIt2 != mergedManifest_.scriptGuidsByType.end()) {
            hoverText += " [Guid=" + gIt2->second + "]";
        }
    }
    return HoverInfo{std::move(hoverText), symbol->location};
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

PrepareRenameResult LanguageService::prepareRename(
    const std::string& rawPath,
    text::LinePosition position) {
    const std::string path = normalizeDocPath(rawPath);
    (void)build();
    const auto* doc = document(path);
    if (!doc) {
        return {false, {}, {}, "document is not open or indexed in workspace"};
    }
    if (isDeclarationStubPath(path) ||
        (ambientDocuments_.find(path) != ambientDocuments_.end() &&
         documents_.find(path) == documents_.end() &&
         workspaceDocuments_.find(path) == workspaceDocuments_.end())) {
        return {false, {}, {}, "cannot rename host C++ ambient declarations (.d.rscript)"};
    }
    const auto token = tokenAt(path, position);
    if (!token) {
        return {false, {}, {}, "no symbol at cursor position"};
    }
    if (token->kind != syntax::SyntaxKind::IdentifierToken) {
        if (syntax::isPrimitiveTypeKeyword(token->kind) ||
            (!token->text.empty() && std::isalpha(static_cast<unsigned char>(token->text.front())))) {
            return {false, {}, {}, "cannot rename language keyword '" + token->text + "'"};
        }
        return {false, {}, {}, "cannot rename literal or punctuation token"};
    }
    const auto isDeclaredInAmbientStub = [&](const std::string& symName) -> bool {
        for (const auto& [ambPath, ambDoc] : ambientDocuments_) {
            if (documents_.find(ambPath) != documents_.end() ||
                workspaceDocuments_.find(ambPath) != workspaceDocuments_.end()) {
                continue;
            }
            for (const auto& t : documentTokens(ambPath, ambDoc)) {
                if (t.kind == syntax::SyntaxKind::IdentifierToken && t.text == symName) {
                    return true;
                }
            }
        }
        for (const auto& wsPath : documentPaths()) {
            if (!isDeclarationStubPath(wsPath)) continue;
            if (const auto* dPtr = document(wsPath)) {
                for (const auto& t : documentTokens(wsPath, *dPtr)) {
                    if (t.kind == syntax::SyntaxKind::IdentifierToken && t.text == symName) {
                        return true;
                    }
                }
            }
        }
        return false;
    };

    const auto symbol = resolve(path, position);
    if (!symbol) {
        if (isDeclaredInAmbientStub(token->text)) {
            return {false, {}, {}, "cannot rename host C++ ambient symbol '" + token->text + "' (.d.rscript)"};
        }
        return {false, {}, {}, "cannot resolve symbol '" + token->text + "' for rename"};
    }
    if (isDeclarationStubPath(symbol->location.path) ||
        (ambientDocuments_.find(symbol->location.path) != ambientDocuments_.end() &&
         documents_.find(symbol->location.path) == documents_.end() &&
         workspaceDocuments_.find(symbol->location.path) == workspaceDocuments_.end())) {
        return {false, {}, {}, "cannot rename host C++ ambient symbol '" + symbol->name + "' declared in '" + symbol->location.path + "'"};
    }
    // Guard call-site fallback when ambient function has no user .rscript definition occurrence
    bool hasUserDefinition = false;
    for (const auto& occ : result_.symbols) {
        if (occ.id == symbol->id && occ.definition && !isDeclarationStubPath(occ.sourceName) &&
            (documents_.find(occ.sourceName) != documents_.end() ||
             workspaceDocuments_.find(occ.sourceName) != workspaceDocuments_.end())) {
            hasUserDefinition = true;
            break;
        }
    }
    if (!hasUserDefinition) {
        for (const auto& def : allDefinitionsRef()) {
            if (def.id == symbol->id && !isDeclarationStubPath(def.location.path) &&
                (documents_.find(def.location.path) != documents_.end() ||
                 workspaceDocuments_.find(def.location.path) != workspaceDocuments_.end())) {
                hasUserDefinition = true;
                break;
            }
        }
    }
    if (!hasUserDefinition && isDeclaredInAmbientStub(token->text)) {
        return {false, {}, {}, "cannot rename host C++ ambient symbol '" + token->text + "' (.d.rscript)"};
    }
    return {true, rangeFor(path, token->span), token->text, {}};
}

std::vector<TextEdit> LanguageService::rename(
    const std::string& path,
    text::LinePosition position,
    const std::string& newName) {
    if (!identifier(newName)) return {};
    const auto prep = prepareRename(path, position);
    if (!prep.valid) return {};
    std::vector<TextEdit> edits;
    for (const auto& location : references(path, position, true)) {
        if (isDeclarationStubPath(location.path)) continue;
        edits.push_back({location, newName});
    }
    return edits;
}

namespace {

bool rangesOverlap(const debug::SourceRange& a, const debug::SourceRange& b) noexcept {
    const auto before = [](text::LinePosition lhs, text::LinePosition rhs) noexcept {
        return lhs.line != rhs.line ? (lhs.line < rhs.line) : (lhs.column < rhs.column);
    };
    if (before(a.end, b.start) || before(b.end, a.start)) return false;
    return true;
}

std::string defaultReturnStatementForType(
    semantic::PrimitiveType prim,
    const std::string& typeName) {
    const std::string t = typeDetail(prim, typeName);
    if (t == "void" || prim == semantic::PrimitiveType::Void) return {};
    if (t == "bool" || prim == semantic::PrimitiveType::Bool) return "return false;";
    if (t == "double" || prim == semantic::PrimitiveType::Double ||
        t == "float" || prim == semantic::PrimitiveType::Float) {
        return "return 0.0;";
    }
    if (t == "string" || prim == semantic::PrimitiveType::String) return "return \"\";";
    if (t == "int" || t == "long" || t == "byte" || t == "sbyte" ||
        t == "short" || t == "ushort" || t == "uint" || t == "ulong" ||
        semantic::isNumericType(prim)) {
        return "return 0;";
    }
    if (t == "Fixed") return "return Fixed.FromInt(0);";
    if (t == "Vec2") return "return Vec2.Zero();";
    if (t == "Vec3") return "return Vec3.Zero();";
    return "return null;";
}

bool unescapeManifestValue(std::string_view raw, std::string& out) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.remove_prefix(1);
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        out.clear();
        for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 2 < raw.size()) {
                ++i;
                switch (raw[i]) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                default: out.push_back(raw[i]); break;
                }
            } else {
                out.push_back(raw[i]);
            }
        }
        return true;
    }
    out.assign(raw.data(), raw.size());
    return !out.empty();
}

} // namespace

std::vector<CodeAction> LanguageService::codeActions(
    const std::string& rawPath,
    debug::SourceRange range) {
    const std::string path = normalizeDocPath(rawPath);
    const auto diags = diagnostics(path);
    const auto* doc = document(path);
    if (!doc) return {};

    std::vector<CodeAction> actions;
    const auto hdr = parseDocHeader(*doc);
    const auto& allTokens = documentTokens(path, *doc);

    // Compute import insertion point (immediately after last `import ...;` or `module ...;`)
    std::size_t importInsertOffset = 0;
    for (std::size_t i = 0; i < allTokens.size(); ++i) {
        if (allTokens[i].text == "module" || allTokens[i].text == "import") {
            std::size_t j = i + 1;
            while (j < allTokens.size() &&
                   allTokens[j].kind != syntax::SyntaxKind::SemicolonToken &&
                   allTokens[j].kind != syntax::SyntaxKind::EndOfFileToken) {
                ++j;
            }
            if (j < allTokens.size() && allTokens[j].kind == syntax::SyntaxKind::SemicolonToken) {
                std::size_t afterSemi = allTokens[j].span.end();
                if (afterSemi < doc->content.size() && doc->content[afterSemi] == '\r') ++afterSemi;
                if (afterSemi < doc->content.size() && doc->content[afterSemi] == '\n') ++afterSemi;
                importInsertOffset = afterSemi;
                i = j;
            }
        } else if (allTokens[i].kind != syntax::SyntaxKind::OpenBracketToken) {
            break;
        }
    }

    // 1. Quick Fix: Auto-import missing module for unresolved types / functions
    std::set<std::string> candidateMissingNames;
    for (const auto& d : diags) {
        if (!rangesOverlap(d.location.range, range)) continue;
        std::size_t q1 = d.message.find('\'');
        while (q1 != std::string::npos) {
            const std::size_t q2 = d.message.find('\'', q1 + 1);
            if (q2 == std::string::npos) break;
            const std::string tokName = d.message.substr(q1 + 1, q2 - q1 - 1);
            if (identifier(tokName)) candidateMissingNames.insert(tokName);
            q1 = d.message.find('\'', q2 + 1);
        }
    }
    if (const auto tok = tokenAt(path, range.start);
        tok && tok->kind == syntax::SyntaxKind::IdentifierToken) {
        if (!resolve(path, range.start)) {
            candidateMissingNames.insert(tok->text);
        }
    }

    if (!candidateMissingNames.empty()) {
        std::set<std::pair<std::string, std::string>> emittedImports; // (moduleName, symbolName)
        const auto considerExport = [&](const std::string& modName, const std::string& symName) {
            if (modName.empty() || modName == "$global" || modName == hdr.moduleName) return;
            if (hdr.imports.find(modName) != hdr.imports.end()) return;
            if (candidateMissingNames.find(symName) == candidateMissingNames.end()) return;
            if (!emittedImports.emplace(modName, symName).second) return;

            const auto insertRange = rangeFor(path, text::TextSpan{importInsertOffset, 0});
            CodeAction act;
            act.title = "Import '" + modName + "' for '" + symName + "'";
            act.kind = "quickfix";
            act.isPreferred = emittedImports.size() == 1;
            act.edits.push_back(TextEdit{
                Location{path, insertRange},
                "import " + modName + ";\n",
            });
            actions.push_back(std::move(act));
        };

        for (const auto& module : result_.modules) {
            for (const auto& type : module.types) {
                if (!type.synthetic && type.accessibility == semantic::Accessibility::Public) {
                    considerExport(module.name, type.name);
                }
            }
            for (const auto& fn : module.functions) {
                if (fn.name.find('$') == std::string::npos && fn.name.find("__") == std::string::npos) {
                    considerExport(module.name, fn.name);
                }
            }
        }
        for (const auto& [modName, cachedMod] : snapshot_.modules) {
            for (const auto& type : cachedMod.module.types) {
                if (!type.synthetic && type.accessibility == semantic::Accessibility::Public) {
                    considerExport(modName, type.name);
                }
            }
            for (const auto& fn : cachedMod.module.functions) {
                if (fn.name.find('$') == std::string::npos && fn.name.find("__") == std::string::npos) {
                    considerExport(modName, fn.name);
                }
            }
        }
    }

    // 2. Quick Fix: Implement missing interface method stubs on class declarations
    struct ParsedIfaceMethod {
        std::string returnType;
        std::string name;
        std::vector<std::pair<std::string, std::string>> params; // (type, name)
    };
    std::unordered_map<std::string, std::vector<ParsedIfaceMethod>> ifaceMethodsByName;
    const auto collectCompiledInterface = [&](const semantic::TypeSymbol& t) {
        if (!t.interfaceType) return;
        auto& vec = ifaceMethodsByName[t.name];
        if (!vec.empty()) return;
        for (const auto& m : t.methods) {
            if (m.synthetic) continue;
            ParsedIfaceMethod pm;
            pm.returnType = typeDetail(m.returnType, m.returnTypeName);
            pm.name = m.name;
            const std::size_t startIdx = (m.method && !m.staticMethod) ? 1 : 0;
            for (std::size_t pi = startIdx; pi < m.parameters.size(); ++pi) {
                pm.params.emplace_back(
                    typeDetail(m.parameters[pi].type, m.parameters[pi].typeName),
                    m.parameters[pi].name.empty() ? ("arg" + std::to_string(pi)) : m.parameters[pi].name);
            }
            vec.push_back(std::move(pm));
        }
        ifaceMethodsByName[semantic::canonicalTypeName(t)] = vec;
    };
    for (const auto& mod : result_.modules) {
        for (const auto& t : mod.types) collectCompiledInterface(t);
    }
    for (const auto& [mName, cachedMod] : snapshot_.modules) {
        (void)mName;
        for (const auto& t : cachedMod.module.types) collectCompiledInterface(t);
    }

    // Fallback token scan for interfaces declared in open/workspace/ambient docs (resilient when current module has errors)
    const auto scanDocForInterfaces = [&](const std::string& dPath, const Document& dDoc) {
        const auto& toks = documentTokens(dPath, dDoc);
        for (std::size_t i = 0; i + 3 < toks.size(); ++i) {
            if (toks[i].text != "interface") continue;
            if (toks[i + 1].kind != syntax::SyntaxKind::IdentifierToken) continue;
            const std::string ifName = toks[i + 1].text;
            if (ifaceMethodsByName.find(ifName) != ifaceMethodsByName.end()) continue;
            std::size_t b = i + 2;
            while (b < toks.size() && toks[b].kind != syntax::SyntaxKind::OpenBraceToken &&
                   toks[b].kind != syntax::SyntaxKind::SemicolonToken) {
                ++b;
            }
            if (b >= toks.size() || toks[b].kind != syntax::SyntaxKind::OpenBraceToken) continue;
            std::vector<ParsedIfaceMethod> methods;
            int depth = 1;
            std::size_t cur = b + 1;
            while (cur < toks.size() && depth > 0) {
                if (toks[cur].kind == syntax::SyntaxKind::OpenBraceToken) { ++depth; ++cur; continue; }
                if (toks[cur].kind == syntax::SyntaxKind::CloseBraceToken) { --depth; ++cur; continue; }
                if (depth == 1) {
                    // Parse `<RetType> <MethodName>(<Params>);`
                    if (toks[cur].text == "public") ++cur;
                    std::size_t pOpen = cur;
                    while (pOpen < toks.size() &&
                           toks[pOpen].kind != syntax::SyntaxKind::OpenParenToken &&
                           toks[pOpen].kind != syntax::SyntaxKind::SemicolonToken &&
                           toks[pOpen].kind != syntax::SyntaxKind::CloseBraceToken) {
                        ++pOpen;
                    }
                    if (pOpen >= cur + 2 && pOpen < toks.size() &&
                        toks[pOpen].kind == syntax::SyntaxKind::OpenParenToken &&
                        toks[pOpen - 1].kind == syntax::SyntaxKind::IdentifierToken) {
                        ParsedIfaceMethod pm;
                        pm.name = toks[pOpen - 1].text;
                        for (std::size_t rt = cur; rt + 1 < pOpen; ++rt) {
                            pm.returnType += toks[rt].text;
                        }
                        std::size_t pClose = pOpen + 1;
                        int pDepth = 1;
                        std::vector<syntax::SyntaxToken> paramToks;
                        while (pClose < toks.size() && pDepth > 0) {
                            if (toks[pClose].kind == syntax::SyntaxKind::OpenParenToken) ++pDepth;
                            else if (toks[pClose].kind == syntax::SyntaxKind::CloseParenToken) --pDepth;
                            if (pDepth > 0) paramToks.push_back(toks[pClose]);
                            ++pClose;
                        }
                        std::size_t ps = 0;
                        std::size_t argIdx = 0;
                        while (ps < paramToks.size()) {
                            std::size_t pe = ps;
                            int gDepth = 0;
                            while (pe < paramToks.size()) {
                                if (paramToks[pe].text == "<") ++gDepth;
                                else if (paramToks[pe].text == ">" && gDepth > 0) --gDepth;
                                else if (paramToks[pe].kind == syntax::SyntaxKind::CommaToken && gDepth == 0) break;
                                ++pe;
                            }
                            if (pe >= ps + 2) {
                                std::string pType;
                                for (std::size_t k = ps; k + 1 < pe; ++k) pType += paramToks[k].text;
                                pm.params.emplace_back(std::move(pType), paramToks[pe - 1].text);
                            } else if (pe == ps + 1) {
                                pm.params.emplace_back(paramToks[ps].text, "arg" + std::to_string(argIdx));
                            }
                            ++argIdx;
                            ps = pe + 1;
                        }
                        methods.push_back(std::move(pm));
                        cur = pClose;
                        while (cur < toks.size() && toks[cur].kind != syntax::SyntaxKind::SemicolonToken &&
                               toks[cur].kind != syntax::SyntaxKind::CloseBraceToken) {
                            ++cur;
                        }
                        if (cur < toks.size() && toks[cur].kind == syntax::SyntaxKind::SemicolonToken) ++cur;
                        continue;
                    }
                }
                ++cur;
            }
            if (!methods.empty()) ifaceMethodsByName[ifName] = std::move(methods);
        }
    };
    scanDocForInterfaces(path, *doc);
    for (const auto& [dPath, dDoc] : documents_) scanDocForInterfaces(dPath, dDoc);
    for (const auto& [dPath, dDoc] : workspaceDocuments_) {
        if (documents_.find(dPath) == documents_.end()) scanDocForInterfaces(dPath, dDoc);
    }
    for (const auto& [dPath, dDoc] : ambientDocuments_) scanDocForInterfaces(dPath, dDoc);

    // Inspect classes in `path` overlapping `range`
    for (std::size_t i = 0; i + 4 < allTokens.size(); ++i) {
        if (allTokens[i].text != "class" && allTokens[i].text != "struct") continue;
        if (allTokens[i + 1].kind != syntax::SyntaxKind::IdentifierToken) continue;
        const std::string className = allTokens[i + 1].text;
        std::size_t colonIdx = i + 2;
        if (colonIdx < allTokens.size() && allTokens[colonIdx].text == "<") {
            int gDepth = 1;
            ++colonIdx;
            while (colonIdx < allTokens.size() && gDepth > 0) {
                if (allTokens[colonIdx].text == "<") ++gDepth;
                else if (allTokens[colonIdx].text == ">") --gDepth;
                ++colonIdx;
            }
        }
        if (colonIdx >= allTokens.size() || allTokens[colonIdx].kind != syntax::SyntaxKind::ColonToken) continue;

        std::vector<std::string> baseList;
        std::size_t cur = colonIdx + 1;
        std::string curBase;
        while (cur < allTokens.size() &&
               allTokens[cur].kind != syntax::SyntaxKind::OpenBraceToken &&
               allTokens[cur].text != "where") {
            if (allTokens[cur].kind == syntax::SyntaxKind::CommaToken) {
                if (!curBase.empty()) { baseList.push_back(curBase); curBase.clear(); }
            } else {
                curBase += allTokens[cur].text;
            }
            ++cur;
        }
        if (!curBase.empty()) baseList.push_back(curBase);
        while (cur < allTokens.size() && allTokens[cur].kind != syntax::SyntaxKind::OpenBraceToken) ++cur;
        if (cur >= allTokens.size()) continue;

        const std::size_t openBraceIdx = cur;
        int braceDepth = 1;
        std::size_t closeBraceIdx = openBraceIdx + 1;
        std::unordered_set<std::string> declaredMethodNames;
        while (closeBraceIdx < allTokens.size() && braceDepth > 0) {
            const auto k = allTokens[closeBraceIdx].kind;
            if (k == syntax::SyntaxKind::OpenBraceToken) {
                ++braceDepth;
            } else if (k == syntax::SyntaxKind::CloseBraceToken) {
                --braceDepth;
                if (braceDepth == 0) break;
            } else if (braceDepth == 1 && k == syntax::SyntaxKind::OpenParenToken && closeBraceIdx > openBraceIdx + 1) {
                if (allTokens[closeBraceIdx - 1].kind == syntax::SyntaxKind::IdentifierToken) {
                    declaredMethodNames.insert(allTokens[closeBraceIdx - 1].text);
                }
            }
            ++closeBraceIdx;
        }
        if (closeBraceIdx >= allTokens.size()) continue;

        const auto classRange = rangeFor(
            path,
            text::TextSpan{
                allTokens[i].span.start,
                allTokens[closeBraceIdx].span.end() - allTokens[i].span.start});
        if (!rangesOverlap(classRange, range)) continue;

        for (const auto& baseName : baseList) {
            const auto ifIt = ifaceMethodsByName.find(baseName);
            if (ifIt == ifaceMethodsByName.end()) continue;
            std::vector<const ParsedIfaceMethod*> missing;
            for (const auto& im : ifIt->second) {
                if (declaredMethodNames.find(im.name) == declaredMethodNames.end()) {
                    missing.push_back(&im);
                }
            }
            if (missing.empty()) continue;

            std::string stubs;
            const std::size_t insertPos = allTokens[closeBraceIdx].span.start;
            if (insertPos > 0 && doc->content[insertPos - 1] != '\n') {
                stubs += "\n";
            }
            for (const auto* im : missing) {
                stubs += "    public " + im->returnType + " " + im->name + "(";
                for (std::size_t pi = 0; pi < im->params.size(); ++pi) {
                    if (pi > 0) stubs += ", ";
                    stubs += im->params[pi].first + " " + im->params[pi].second;
                }
                stubs += ")\n    {\n";
                const std::string retStmt =
                    defaultReturnStatementForType(semantic::PrimitiveType::Error, im->returnType);
                if (!retStmt.empty()) {
                    stubs += "        " + retStmt + "\n";
                }
                stubs += "    }\n";
            }

            const auto editRange = rangeFor(path, text::TextSpan{insertPos, 0});
            CodeAction act;
            act.title = "Implement interface '" + baseName + "' in '" + className + "'";
            act.kind = "quickfix";
            act.isPreferred = true;
            act.edits.push_back(TextEdit{Location{path, editRange}, std::move(stubs)});
            actions.push_back(std::move(act));
        }
    }

    // 3. Quick Fix: Insert `// realscript-disable-next-line <RSG_CODE>` for RSG* governance diagnostics
    {
        std::vector<LanguageDiagnostic> rsgCandidates;
        for (const auto& d : diags) {
            if (d.code.rfind("RSG", 0) == 0 && rangesOverlap(d.location.range, range)) {
                rsgCandidates.push_back(d);
            }
        }
        if (!lintConfig_.enableGovernanceLint) {
            for (const auto& d : lint(path, lintConfig_)) {
                if (d.code.rfind("RSG", 0) == 0 && rangesOverlap(d.location.range, range)) {
                    rsgCandidates.push_back(d);
                }
            }
        }

        std::set<std::pair<std::string, std::size_t>> seenRsgFix;
        for (const auto& d : rsgCandidates) {
            const std::size_t line0 = d.location.range.start.line;
            if (!seenRsgFix.emplace(d.code, line0).second) continue;
            if (line0 >= doc->fileInfo.lineStarts.size()) continue;

            const std::size_t lineStartOff = static_cast<std::size_t>(doc->fileInfo.lineStarts[line0]);
            std::size_t indentEnd = lineStartOff;
            while (indentEnd < doc->content.size() &&
                   (doc->content[indentEnd] == ' ' || doc->content[indentEnd] == '\t')) {
                ++indentEnd;
            }
            const std::string lineIndent = doc->content.substr(lineStartOff, indentEnd - lineStartOff);

            // Check if the immediately preceding line already has a `// realscript-disable-next-line` directive
            bool appendedToExisting = false;
            TextEdit fixEdit;
            if (line0 > 0) {
                const std::size_t prevStart = static_cast<std::size_t>(doc->fileInfo.lineStarts[line0 - 1]);
                std::size_t prevEnd = lineStartOff;
                while (prevEnd > prevStart &&
                       (doc->content[prevEnd - 1] == '\n' || doc->content[prevEnd - 1] == '\r')) {
                    --prevEnd;
                }
                std::string_view prevLine(doc->content.data() + prevStart, prevEnd - prevStart);
                while (!prevLine.empty() && (prevLine.front() == ' ' || prevLine.front() == '\t')) {
                    prevLine.remove_prefix(1);
                }
                if (prevLine.rfind("//", 0) == 0) {
                    std::string_view body = prevLine.substr(2);
                    while (!body.empty() && (body.front() == ' ' || body.front() == '\t')) {
                        body.remove_prefix(1);
                    }
                    constexpr std::string_view kDir = "realscript-disable-next-line";
                    if (body.rfind(kDir, 0) == 0 &&
                        body.size() > kDir.size() &&
                        body.find("--") == std::string_view::npos) {
                        fixEdit = TextEdit{
                            Location{path, rangeFor(path, text::TextSpan{prevEnd, 0})},
                            ", " + d.code,
                        };
                        appendedToExisting = true;
                    }
                }
            }
            if (!appendedToExisting) {
                fixEdit = TextEdit{
                    Location{path, rangeFor(path, text::TextSpan{lineStartOff, 0})},
                    lineIndent + "// realscript-disable-next-line " + d.code + "\n",
                };
            }

            CodeAction act;
            act.title = "Suppress " + d.code + " with '// realscript-disable-next-line " + d.code + "'";
            act.kind = "quickfix";
            act.isPreferred = actions.empty();
            act.edits.push_back(std::move(fixEdit));
            actions.push_back(std::move(act));
        }
    }

    return actions;
}

void LanguageService::refreshSceneManifest() {
    sceneDiskFiles_.clear();
    for (const auto& rootPath : workspaceRoots_) {
        std::error_code ec;
        const std::filesystem::path fsRoot(rootPath);
        if (!std::filesystem::is_directory(fsRoot, ec) || ec) continue;
        const std::filesystem::path rootCfg = fsRoot / "realscript.project.json";
        if (std::filesystem::is_regular_file(rootCfg, ec) && !ec) {
            (void)loadProjectConfigFile(rootCfg.generic_string(), false);
        }
        std::filesystem::recursive_directory_iterator it(
            fsRoot, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        while (!ec && it != end) {
            const auto& entry = *it;
            std::error_code eec;
            if (entry.is_directory(eec) && !eec) {
                if (shouldSkipDirectoryName(entry.path().filename().string())) {
                    it.disable_recursion_pending();
                }
                it.increment(ec);
                continue;
            }
            if (entry.is_regular_file(eec) && !eec && hasSceneOrPrefabExtension(entry.path())) {
                std::string text;
                if (readFileContents(entry.path(), text)) {
                    sceneDiskFiles_[normalizeDocPath(entry.path().generic_string())] = std::move(text);
                }
            }
            it.increment(ec);
        }
    }
    manifestDirty_ = true;
}

SceneAssetManifest LanguageService::sceneManifest() const {
    if (dirty_) {
        const_cast<LanguageService*>(this)->rebuild();
    }
    rebuildSceneManifestIfNeeded();
    return mergedManifest_;
}

void LanguageService::rebuildSceneManifestIfNeeded() const {
    if (!manifestDirty_) return;
    mergedManifest_ = {};
    mergedManifest_.entities = configManifest_.entities;
    mergedManifest_.signals = configManifest_.signals;
    mergedManifest_.resources = configManifest_.resources;

    std::set<std::string> seenEntities;
    for (const auto& e : mergedManifest_.entities) seenEntities.insert(e.name + "|" + e.nodePath);
    std::set<std::string> seenSignals;
    for (const auto& s : mergedManifest_.signals) seenSignals.insert(s.name);
    std::set<std::string> seenResources;
    for (const auto& r : mergedManifest_.resources) seenResources.insert(r.path);

    // 1. Extract [Guid("...")] and [Signal] / event declarations from compiled & source metadata
    for (const auto& attr : result_.nativeAttributes) {
        if (attr.name == "Guid" && !attr.arguments.empty() && !attr.arguments.front().value.empty()) {
            std::string gVal = attr.arguments.front().value;
            if (gVal.size() >= 2 && gVal.front() == '"' && gVal.back() == '"') {
                gVal = gVal.substr(1, gVal.size() - 2);
            }
            mergedManifest_.scriptGuidsByType[attr.target] = gVal;
            const auto sep = attr.target.rfind("::");
            if (sep != std::string::npos) {
                mergedManifest_.scriptGuidsByType[attr.target.substr(sep + 2)] = gVal;
            }
        } else if (attr.name == "Signal") {
            std::string sigName;
            if (!attr.arguments.empty() && !attr.arguments.front().value.empty()) {
                sigName = attr.arguments.front().value;
                if (sigName.size() >= 2 && sigName.front() == '"' && sigName.back() == '"') {
                    sigName = sigName.substr(1, sigName.size() - 2);
                }
            } else {
                const auto dot = attr.target.rfind('.');
                const auto col = attr.target.rfind("::");
                const std::size_t cut = (dot != std::string::npos) ? (dot + 1)
                    : ((col != std::string::npos) ? (col + 2) : 0);
                sigName = attr.target.substr(cut);
            }
            if (!sigName.empty() && seenSignals.insert(sigName).second) {
                mergedManifest_.signals.push_back(SignalInfo{sigName, attr.target, {}, attr.sourceName});
            }
        }
    }
    for (const auto& mod : result_.modules) {
        for (const auto& t : mod.types) {
            if (t.synthetic) continue;
            const std::string canon = semantic::canonicalTypeName(t);
            if (mergedManifest_.scriptGuidsByType.find(canon) == mergedManifest_.scriptGuidsByType.end()) {
                const std::string detGuid = game::AssetGuid::fromTypeName(canon).toString();
                mergedManifest_.scriptGuidsByType.emplace(canon, detGuid);
                mergedManifest_.scriptGuidsByType.emplace(t.name, detGuid);
            }
            for (const auto& ev : t.events) {
                if (seenSignals.insert(ev.name).second) {
                    std::string sig = "(";
                    for (std::size_t pi = 0; pi < ev.parameters.size(); ++pi) {
                        if (pi > 0) sig += ", ";
                        sig += typeDetail(ev.parameters[pi].type, ev.parameters[pi].typeName) + " " + ev.parameters[pi].name;
                    }
                    sig += ")";
                    mergedManifest_.signals.push_back(SignalInfo{ev.name, canon, std::move(sig), t.sourceName});
                }
            }
        }
    }

    // 2. Parse workspace .scene.txt / .prefab.txt / .guid.txt files
    const auto computeResUri = [&](const std::string& diskPath) -> std::string {
        for (const auto& root : workspaceRoots_) {
            if (isPathWithinRoot(diskPath, root) && diskPath.size() > root.size()) {
                std::string rel = diskPath.substr(root.size());
                if (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
                return "res://" + rel;
            }
        }
        return "res://" + std::filesystem::path(diskPath).filename().generic_string();
    };

    for (const auto& [filePath, text] : sceneDiskFiles_) {
        const std::string resUri = computeResUri(filePath);
        if (endsWithIgnoreCase(filePath, ".prefab.txt") || endsWithIgnoreCase(filePath, ".scene.txt")) {
            if (seenResources.insert(resUri).second) {
                AssetResourceInfo r;
                r.path = resUri;
                r.guid = game::AssetGuid::fromAssetPath(resUri).toString();
                r.resourceType = endsWithIgnoreCase(filePath, ".prefab.txt") ? "Prefab" : "Scene";
                r.sourceFile = filePath;
                mergedManifest_.resources.push_back(std::move(r));
            }
        }

        // Parse both Sprint 2 key=value format (`entity.<id>.name = "..."`, `node."$root/Child".name = "..."`, `signal.<idx>.name = "..."`)
        // AND bracket header format (`[Entity "Player" id=1 path="/Root/Player" guid="..."]`)
        std::map<std::uint64_t, SceneEntityInfo> kvEntities;
        std::map<std::uint64_t, std::uint64_t> kvParents;
        std::map<std::string, SceneEntityInfo> prefabNodes;
        std::map<std::uint64_t, SignalInfo> kvSignals;
        SceneEntityInfo* activeBracketEnt = nullptr;

        std::istringstream in(text);
        std::string rawLine;
        while (std::getline(in, rawLine)) {
            if (!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();
            std::string_view lv(rawLine);
            while (!lv.empty() && std::isspace(static_cast<unsigned char>(lv.front()))) lv.remove_prefix(1);
            while (!lv.empty() && std::isspace(static_cast<unsigned char>(lv.back()))) lv.remove_suffix(1);
            if (lv.empty() || lv.front() == '#') continue;

            if (lv.front() == '[' && lv.back() == ']') {
                activeBracketEnt = nullptr;
                const std::string hdrLine(lv.substr(1, lv.size() - 2));
                if (hdrLine.rfind("Entity ", 0) == 0) {
                    SceneEntityInfo ent;
                    ent.sourceFile = filePath;
                    const auto q1 = hdrLine.find('"');
                    const auto q2 = (q1 != std::string::npos) ? hdrLine.find('"', q1 + 1) : std::string::npos;
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        ent.name = hdrLine.substr(q1 + 1, q2 - q1 - 1);
                    }
                    const auto extractAttr = [&](const std::string& key) -> std::string {
                        const auto pos = hdrLine.find(key + "=");
                        if (pos == std::string::npos) return {};
                        std::size_t vStart = pos + key.size() + 1;
                        if (vStart < hdrLine.size() && hdrLine[vStart] == '"') {
                            const auto vEnd = hdrLine.find('"', vStart + 1);
                            if (vEnd != std::string::npos) return hdrLine.substr(vStart + 1, vEnd - vStart - 1);
                        }
                        std::size_t vEnd = vStart;
                        while (vEnd < hdrLine.size() && !std::isspace(static_cast<unsigned char>(hdrLine[vEnd]))) ++vEnd;
                        return hdrLine.substr(vStart, vEnd - vStart);
                    };
                    if (const auto idStr = extractAttr("id"); !idStr.empty()) {
                        ent.entityId = static_cast<std::uint64_t>(std::strtoull(idStr.c_str(), nullptr, 10));
                    }
                    ent.guid = extractAttr("guid");
                    ent.nodePath = extractAttr("path");
                    if (ent.nodePath.empty() && !ent.name.empty()) ent.nodePath = "/" + ent.name;
                    if (!ent.name.empty() && seenEntities.insert(ent.name + "|" + ent.nodePath).second) {
                        mergedManifest_.entities.push_back(std::move(ent));
                        activeBracketEnt = &mergedManifest_.entities.back();
                    }
                } else if (hdrLine.rfind("Signal ", 0) == 0) {
                    const auto q1 = hdrLine.find('"');
                    const auto q2 = (q1 != std::string::npos) ? hdrLine.find('"', q1 + 1) : std::string::npos;
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        const std::string sName = hdrLine.substr(q1 + 1, q2 - q1 - 1);
                        if (seenSignals.insert(sName).second) {
                            mergedManifest_.signals.push_back(SignalInfo{sName, {}, {}, filePath});
                        }
                    }
                } else if (hdrLine.rfind("Resource ", 0) == 0) {
                    const auto q1 = hdrLine.find('"');
                    const auto q2 = (q1 != std::string::npos) ? hdrLine.find('"', q1 + 1) : std::string::npos;
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        const std::string rPath = hdrLine.substr(q1 + 1, q2 - q1 - 1);
                        if (seenResources.insert(rPath).second) {
                            AssetResourceInfo r;
                            r.path = rPath;
                            r.guid = game::AssetGuid::fromAssetPath(rPath).toString();
                            r.sourceFile = filePath;
                            mergedManifest_.resources.push_back(std::move(r));
                        }
                    }
                }
                continue;
            }

            const auto eqPos = lv.find('=');
            if (eqPos == std::string_view::npos) continue;
            std::string_view kView = lv.substr(0, eqPos);
            std::string_view vView = lv.substr(eqPos + 1);
            while (!kView.empty() && std::isspace(static_cast<unsigned char>(kView.back()))) kView.remove_suffix(1);
            while (!vView.empty() && std::isspace(static_cast<unsigned char>(vView.front()))) vView.remove_prefix(1);

            if (activeBracketEnt && kView == "component") {
                std::string compName;
                if (unescapeManifestValue(vView, compName)) {
                    activeBracketEnt->components.push_back(std::move(compName));
                }
                continue;
            }

            // Native Sprint 2 scene text: `entity.<id>.name = "Hero"` / `entity.<id>.component.<guid>.type = "Game::Hero"`
            if (kView.rfind("entity.", 0) == 0) {
                const auto rest = kView.substr(7);
                const auto dot = rest.find('.');
                if (dot != std::string_view::npos) {
                    const std::string idTok(rest.substr(0, dot));
                    const auto eid = static_cast<std::uint64_t>(std::strtoull(idTok.c_str(), nullptr, 10));
                    const auto prop = rest.substr(dot + 1);
                    auto& ent = kvEntities[eid];
                    ent.entityId = eid;
                    ent.sourceFile = filePath;
                    if (prop == "name") {
                        (void)unescapeManifestValue(vView, ent.name);
                    } else if (prop == "parent") {
                        const std::string pTok(vView);
                        const auto pid = static_cast<std::uint64_t>(std::strtoull(pTok.c_str(), nullptr, 10));
                        kvParents[eid] = pid;
                    } else if (prop.rfind("component.", 0) == 0 && endsWithIgnoreCase(prop, ".type")) {
                        std::string compType;
                        if (unescapeManifestValue(vView, compType)) {
                            ent.components.push_back(std::move(compType));
                        }
                    }
                }
                continue;
            }

            // Native Sprint 2 prefab text: `node."$root/Weapon".name = "Weapon"`
            if (kView.rfind("node.\"", 0) == 0) {
                std::size_t qEnd = std::string_view::npos;
                for (std::size_t qi = 6; qi < kView.size(); ++qi) {
                    if (kView[qi] == '\\') ++qi;
                    else if (kView[qi] == '"') { qEnd = qi; break; }
                }
                if (qEnd != std::string_view::npos && qEnd + 1 < kView.size() && kView[qEnd + 1] == '.') {
                    std::string nodeKey;
                    if (unescapeManifestValue(kView.substr(5, qEnd - 4), nodeKey)) {
                        const auto prop = kView.substr(qEnd + 2);
                        auto& pEnt = prefabNodes[nodeKey];
                        pEnt.nodePath = nodeKey;
                        pEnt.sourceFile = filePath;
                        if (prop == "name") {
                            (void)unescapeManifestValue(vView, pEnt.name);
                        } else if (prop.rfind("component.", 0) == 0 && endsWithIgnoreCase(prop, ".type")) {
                            std::string compType;
                            if (unescapeManifestValue(vView, compType)) {
                                pEnt.components.push_back(std::move(compType));
                            }
                        }
                    }
                }
                continue;
            }

            // Native Sprint 2 wired signals: `signal.<idx>.name = "on_damaged"` / `signal.<idx>.src_comp = "..."`
            if (kView.rfind("signal.", 0) == 0) {
                const auto rest = kView.substr(7);
                const auto dot = rest.find('.');
                if (dot != std::string_view::npos) {
                    const std::string idxTok(rest.substr(0, dot));
                    const auto sIdx = static_cast<std::uint64_t>(std::strtoull(idxTok.c_str(), nullptr, 10));
                    const auto prop = rest.substr(dot + 1);
                    auto& sig = kvSignals[sIdx];
                    sig.sourceFile = filePath;
                    if (prop == "name") (void)unescapeManifestValue(vView, sig.name);
                    else if (prop == "src_comp") (void)unescapeManifestValue(vView, sig.ownerType);
                }
                continue;
            }

            // Native Sprint 2 AssetGuid registry text: `asset.<guid> = "res://..."`
            if (kView.rfind("asset.", 0) == 0) {
                const std::string guidStr(kView.substr(6));
                std::string assetPath;
                if (unescapeManifestValue(vView, assetPath) && seenResources.insert(assetPath).second) {
                    mergedManifest_.resources.push_back(AssetResourceInfo{assetPath, guidStr, "Asset", filePath});
                }
            }
        }

        // Finalize hierarchy nodePaths for Sprint 2 kvEntities (two passes to avoid moving parent names early)
        for (auto& [eid, ent] : kvEntities) {
            if (ent.name.empty()) ent.name = "Entity_" + std::to_string(eid);
        }
        for (auto& [eid, ent] : kvEntities) {
            std::vector<std::string> chain{ent.name};
            std::uint64_t cur = eid;
            std::unordered_set<std::uint64_t> guard{cur};
            while (kvParents.find(cur) != kvParents.end()) {
                const auto p = kvParents[cur];
                if (p == 0 || p == ~std::uint64_t{0} || !guard.insert(p).second) break;
                const auto pIt = kvEntities.find(p);
                if (pIt == kvEntities.end()) break;
                chain.push_back(pIt->second.name);
                cur = p;
            }
            std::string fullPath;
            for (auto rIt = chain.rbegin(); rIt != chain.rend(); ++rIt) {
                fullPath += "/" + *rIt;
            }
            ent.nodePath = std::move(fullPath);
        }
        for (auto& [eid, ent] : kvEntities) {
            (void)eid;
            if (seenEntities.insert(ent.name + "|" + ent.nodePath).second) {
                mergedManifest_.entities.push_back(std::move(ent));
            }
        }
        for (auto& [nKey, pEnt] : prefabNodes) {
            (void)nKey;
            if (pEnt.name.empty()) pEnt.name = pEnt.nodePath;
            if (!pEnt.nodePath.empty() && pEnt.nodePath.rfind("$root", 0) == 0) {
                pEnt.nodePath = pEnt.nodePath.substr(5);
                if (pEnt.nodePath.empty()) pEnt.nodePath = "/" + pEnt.name;
            }
            if (seenEntities.insert(pEnt.name + "|" + pEnt.nodePath).second) {
                mergedManifest_.entities.push_back(std::move(pEnt));
            }
        }
        for (auto& [sIdx, sig] : kvSignals) {
            (void)sIdx;
            if (!sig.name.empty() && seenSignals.insert(sig.name).second) {
                mergedManifest_.signals.push_back(std::move(sig));
            }
        }
    }

    manifestDirty_ = false;
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
    FormatOptions opts;
    opts.indentWidth = std::max<std::size_t>(1, tabSize);
    return formatDocument(rawPath, opts);
}

std::vector<TextEdit> LanguageService::formatDocument(
    const std::string& rawPath,
    const FormatOptions& options) {
    const std::string path = normalizeDocPath(rawPath);
    const auto* doc = document(path);
    if (!doc) return {};

    const auto result = formatSource(doc->content, options);
    if (result.unchangedOnError || !result.changed || result.formatted == doc->content) {
        return {};
    }
    const auto fullRange = rangeFor(path, text::TextSpan{0, doc->content.size()});
    return {TextEdit{Location{path, fullRange}, result.formatted}};
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
