#include "realscript/tooling/LanguageService.h"

#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"
#include "realscript/tooling/Json.h"

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

bool LanguageService::applyContentChanges(
    const std::string& path,
    const std::vector<ContentChange>& changes,
    std::int64_t version) {
    auto it = documents_.find(path);
    if (it == documents_.end()) {
        if (changes.empty()) return false;
        documents_[path] = {std::string{}, version};
        it = documents_.find(path);
    }
    std::string& content = it->second.content;
    it->second.version = version;

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
    dirty_ = true;
    return true;
}

std::string LanguageService::documentContent(const std::string& path) const {
    const auto* doc = document(path);
    return doc ? doc->content : std::string{};
}

void LanguageService::close(const std::string& path) {
    documents_.erase(path);
    dirty_ = true;
}

void LanguageService::setAmbientSources(std::vector<compiler::SourceFile> sources) {
    ambientDocuments_.clear();
    for (auto& src : sources) {
        ambientDocuments_[std::move(src.path)] = {std::move(src.content), 1};
    }
    dirty_ = true;
}

void LanguageService::addAmbientSource(compiler::SourceFile source) {
    ambientDocuments_[std::move(source.path)] = {std::move(source.content), 1};
    dirty_ = true;
}

void LanguageService::clearAmbientSources() {
    ambientDocuments_.clear();
    dirty_ = true;
}

bool LanguageService::loadProjectConfigJson(const std::string& jsonText) {
    std::string error;
    const auto parsed = Json::parse(jsonText, error);
    if (!parsed || !parsed->isObject()) return false;
    const auto* ambientArr = parsed->find("ambientStubs");
    if (!ambientArr) {
        ambientArr = parsed->find("ambientSources");
    }
    if (!ambientArr) {
        ambientArr = parsed->find("generatedModules");
    }
    if (!ambientArr || !ambientArr->isArray()) return false;
    ambientDocuments_.clear();
    for (const auto& entry : ambientArr->arrayValue()) {
        if (!entry.isObject()) continue;
        const auto* pathVal = entry.find("path");
        const auto* contentVal = entry.find("content");
        if (!pathVal || !pathVal->isString() || !contentVal || !contentVal->isString()) {
            continue;
        }
        ambientDocuments_[pathVal->stringValue()] = {contentVal->stringValue(), 1};
    }
    dirty_ = true;
    return true;
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
    std::vector<std::string> ambientPaths;
    ambientPaths.reserve(ambientDocuments_.size());
    for (const auto& [path, documentValue] : ambientDocuments_) {
        (void)documentValue;
        if (documents_.find(path) == documents_.end()) {
            ambientPaths.push_back(path);
        }
    }
    std::sort(ambientPaths.begin(), ambientPaths.end());
    for (const auto& path : ambientPaths) {
        compilation.addSource({path, ambientDocuments_.at(path).content});
    }

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
    if (result_.diagnostics.hasErrors()) {
        bool anySanitized = false;
        compiler::Compilation fallbackCompilation;
        for (const auto& path : ambientPaths) {
            fallbackCompilation.addSource({path, ambientDocuments_.at(path).content});
        }
        for (const auto& path : paths) {
            std::string content = documents_.at(path).content;
            std::size_t lineStart = 0;
            while (lineStart < content.size()) {
                std::size_t lineEnd = content.find('\n', lineStart);
                if (lineEnd == std::string::npos) lineEnd = content.size();
                std::size_t lastNonWs = lineEnd;
                while (lastNonWs > lineStart &&
                       std::isspace(static_cast<unsigned char>(content[lastNonWs - 1]))) {
                    --lastNonWs;
                }
                if (lastNonWs > lineStart && content[lastNonWs - 1] == '.') {
                    for (std::size_t idx = lineStart; idx < lastNonWs; ++idx) {
                        if (content[idx] != '\r' && content[idx] != '\n') {
                            content[idx] = ' ';
                        }
                    }
                    anySanitized = true;
                }
                lineStart = (lineEnd < content.size()) ? lineEnd + 1 : content.size();
            }
            fallbackCompilation.addSource({path, std::move(content)});
        }
        if (anySanitized) {
            auto fallback = fallbackCompilation.build(nullptr);
            for (auto& mod : fallback.modules) {
                const bool exists = std::any_of(
                    result_.modules.begin(),
                    result_.modules.end(),
                    [&](const mir::Module& existing) { return existing.name == mod.name; });
                if (!exists) {
                    result_.modules.push_back(std::move(mod));
                }
            }
            if (result_.symbols.empty()) {
                result_.symbols = std::move(fallback.symbols);
            } else {
                for (auto& occ : fallback.symbols) {
                    result_.symbols.push_back(std::move(occ));
                }
            }
        }
    }
    snapshot_ = result_.snapshot;
    hasSnapshot_ = true;
    dirty_ = false;
}

const LanguageService::Document* LanguageService::document(const std::string& path) const noexcept {
    const auto found = documents_.find(path);
    if (found != documents_.end()) return &found->second;
    const auto ambientFound = ambientDocuments_.find(path);
    return ambientFound == ambientDocuments_.end() ? nullptr : &ambientFound->second;
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
            (documents_.find(path) == documents_.end() &&
             ambientDocuments_.find(path) == ambientDocuments_.end()) ||
            !identifier(name) || generatedNames.find(name) != generatedNames.end() ||
            !seen.insert(id).second) {
            return;
        }
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

    for (const auto& symbol : definitions) {
        if (symbol.location.path == path &&
            symbol.location.range.span.start == token->span.start &&
            symbol.location.range.span.length == token->span.length) {
            return symbol;
        }
    }

    semantic::SymbolId matchedId = 0;
    const semantic::SymbolOccurrence* matchedOcc = nullptr;
    for (const auto& occ : result_.symbols) {
        if (occ.id == 0 || occ.sourceName != path) continue;
        if (occ.span.start == token->span.start &&
            occ.span.length == token->span.length) {
            matchedId = occ.id;
            matchedOcc = &occ;
            if (occ.definition) break;
        }
    }
    if (matchedId != 0) {
        for (const auto& symbol : definitions) {
            if (symbol.id == matchedId) return symbol;
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
    const auto symbolVisible = [&](semantic::SymbolId id) {
        for (const auto& module : result_.modules) {
            for (const auto& type : module.types) {
                if (type.id == id) {
                    return type.accessibility ==
                            semantic::Accessibility::Public ||
                        type.moduleName == requesterModule;
                }
                for (const auto& field : type.fields) {
                    if (field.id == id) {
                        return accessAllowed(
                            field.accessibility,
                            field.declaringTypeId,
                            type.moduleName);
                    }
                }
                for (const auto& method : type.methods) {
                    if (method.id == id) {
                        return accessAllowed(
                            method.accessibility,
                            method.declaringTypeId,
                            method.moduleName);
                    }
                }
                for (const auto& constructor : type.constructors) {
                    if (constructor.id == id) {
                        return accessAllowed(
                            constructor.accessibility,
                            constructor.declaringTypeId,
                            constructor.moduleName);
                    }
                }
                for (const auto& property : type.properties) {
                    if (property.id == id) {
                        return accessAllowed(
                            property.accessibility,
                            property.declaringTypeId,
                            type.moduleName);
                    }
                }
            }
        }
        return true;
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
                            for (const auto& occ : result_.symbols) {
                                if (occ.definition && occ.sourceName == path &&
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
        "static", "const",
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
    for (const auto& symbol : allDefinitions()) {
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
    const std::string& path,
    text::LinePosition position) {
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

    if (symbol->id != 0) {
        bool hasAnySymbolOccurrence = false;
        std::set<std::tuple<std::string, std::size_t, std::size_t>> seenSpans;
        for (const auto& occ : result_.symbols) {
            if (occ.id != symbol->id) continue;
            if (documents_.find(occ.sourceName) == documents_.end() &&
                ambientDocuments_.find(occ.sourceName) == ambientDocuments_.end()) {
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
        if (includeDefinition &&
            (documents_.find(symbol->location.path) != documents_.end() ||
             ambientDocuments_.find(symbol->location.path) != ambientDocuments_.end())) {
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
