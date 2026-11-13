#include "realscript/tooling/LspServer.h"
#include "realscript/Version.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realscript::tooling {
namespace {

Json position(text::LinePosition value) {
    return Json::Object{{"line", static_cast<std::int64_t>(value.line)},
                        {"character", static_cast<std::int64_t>(value.column)}};
}

Json range(const debug::SourceRange& value) {
    return Json::Object{{"start", position(value.start)}, {"end", position(value.end)}};
}

text::LinePosition readPosition(const Json* value) {
    if (!value) return {};
    const auto* line = value->find("line");
    const auto* character = value->find("character");
    const auto lineValue = line ? line->integerValue() : 0;
    const auto characterValue = character ? character->integerValue() : 0;
    return {
        static_cast<std::size_t>(std::max<std::int64_t>(0, lineValue)),
        static_cast<std::size_t>(std::max<std::int64_t>(0, characterValue)),
    };
}

const Json* nested(const Json& value, const std::initializer_list<const char*>& keys) {
    const Json* current = &value;
    for (const auto* key : keys) {
        current = current ? current->find(key) : nullptr;
    }
    return current;
}

int completionKind(semantic::SymbolKind kind) {
    switch (kind) {
    case semantic::SymbolKind::Type: return 7;
    case semantic::SymbolKind::Function: return 3;
    case semantic::SymbolKind::Field: return 5;
    case semantic::SymbolKind::Property: return 10;
    case semantic::SymbolKind::EnumMember: return 20;
    case semantic::SymbolKind::Parameter:
    case semantic::SymbolKind::Local: return 6;
    case semantic::SymbolKind::Module: return 9;
    }
    return 1;
}

int documentSymbolKind(semantic::SymbolKind kind) {
    switch (kind) {
    case semantic::SymbolKind::Type: return 5;
    case semantic::SymbolKind::Function: return 12;
    case semantic::SymbolKind::Field: return 8;
    case semantic::SymbolKind::Property: return 7;
    case semantic::SymbolKind::EnumMember: return 22;
    case semantic::SymbolKind::Parameter:
    case semantic::SymbolKind::Local: return 13;
    case semantic::SymbolKind::Module: return 2;
    }
    return 13;
}

int lspDiagnosticSeverity(const LanguageDiagnostic& diagnostic) noexcept {
    if (diagnostic.lintSeverity.has_value()) {
        switch (*diagnostic.lintSeverity) {
        case LintSeverity::Error: return 1;
        case LintSeverity::Warning: return 2;
        case LintSeverity::Info: return 3;
        case LintSeverity::Off: return 4;
        }
    }
    return diagnostic.severity == diagnostics::DiagnosticSeverity::Error ? 1 : 2;
}

Json location(const Location& value) {
    return Json::Object{{"uri", pathToUri(value.path)}, {"range", range(value.range)}};
}

std::vector<std::string> extractWorkspaceRoots(const Json* params) {
    std::vector<std::string> roots;
    if (!params || !params->isObject()) return roots;

    if (const auto* wsFolders = params->find("workspaceFolders");
        wsFolders && wsFolders->isArray()) {
        for (const auto& folder : wsFolders->arrayValue()) {
            if (!folder.isObject()) continue;
            if (const auto* uriVal = folder.find("uri"); uriVal && uriVal->isString()) {
                const std::string p = uriToPath(uriVal->stringValue());
                if (!p.empty()) roots.push_back(p);
            }
        }
    }
    if (roots.empty()) {
        if (const auto* rootUri = params->find("rootUri");
            rootUri && rootUri->isString() && !rootUri->stringValue().empty()) {
            roots.push_back(uriToPath(rootUri->stringValue()));
        } else if (const auto* rootPath = params->find("rootPath");
                   rootPath && rootPath->isString() && !rootPath->stringValue().empty()) {
            roots.push_back(rootPath->stringValue());
        }
    }
    return roots;
}

} // namespace

Json LspServer::response(const Json& request, Json result) const {
    Json::Object object{{"jsonrpc", "2.0"}, {"result", std::move(result)}};
    if (const auto* id = request.find("id")) object.emplace("id", *id);
    else object.emplace("id", nullptr);
    return object;
}

Json LspServer::error(const Json& request, int code, std::string message) const {
    Json::Object body{{"code", code}, {"message", std::move(message)}};
    Json::Object object{{"jsonrpc", "2.0"}, {"error", Json(std::move(body))}};
    if (const auto* id = request.find("id")) object.emplace("id", *id);
    else object.emplace("id", nullptr);
    return object;
}

std::string LspServer::resolveDocumentText(const std::string& path) const {
    if (path.empty()) return {};
    std::string content = service_.documentContent(path);
    if (!content.empty() || service_.contains(path)) {
        return content;
    }
    const std::filesystem::path fsPath(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fsPath, ec) || ec) return {};
    if (fsPath.extension() != ".rscript") return {};
    constexpr std::uintmax_t kMaxDocumentReadBytes = 16u * 1024u * 1024u;
    const auto fsize = std::filesystem::file_size(fsPath, ec);
    if (ec || fsize > kMaxDocumentReadBytes) return {};
    std::ifstream in(fsPath, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

text::LinePosition LspServer::fromLspPosition(
    std::string_view documentText,
    const Json* value) const noexcept {
    const auto raw = readPosition(value);
    if (positionEncoding_ == PositionEncoding::Utf8) {
        return raw;
    }
    return text::utf16ToUtf8Position(documentText, raw);
}

Json LspServer::toLspPosition(
    std::string_view documentText,
    text::LinePosition utf8Pos) const {
    if (positionEncoding_ == PositionEncoding::Utf8) {
        return position(utf8Pos);
    }
    return position(text::utf8ToUtf16Position(documentText, utf8Pos));
}

Json LspServer::toLspRange(
    std::string_view documentText,
    const debug::SourceRange& utf8Range) const {
    if (positionEncoding_ == PositionEncoding::Utf8) {
        return range(utf8Range);
    }
    return Json::Object{
        {"start", toLspPosition(documentText, utf8Range.start)},
        {"end", toLspPosition(documentText, utf8Range.end)},
    };
}

Json LspServer::toLspLocation(const Location& value) const {
    if (positionEncoding_ == PositionEncoding::Utf8) {
        return location(value);
    }
    const std::string docText = resolveDocumentText(value.path);
    return Json::Object{
        {"uri", pathToUri(value.path)},
        {"range", toLspRange(docText, value.range)},
    };
}

void LspServer::publishDiagnostics(const std::string& path, std::ostream& output) {
    Json::Array values;
    const auto diags = service_.diagnostics(path);
    const std::string docText = positionEncoding_ == PositionEncoding::Utf16
        ? resolveDocumentText(path)
        : std::string{};
    std::unordered_map<std::string, std::string> docCache;
    for (const auto& diagnostic : diags) {
        std::string_view diagDocView = docText;
        if (positionEncoding_ == PositionEncoding::Utf16 &&
            !diagnostic.location.path.empty() &&
            diagnostic.location.path != path) {
            auto [it, inserted] = docCache.try_emplace(diagnostic.location.path);
            if (inserted) {
                it->second = resolveDocumentText(diagnostic.location.path);
            }
            diagDocView = it->second;
        }
        values.emplace_back(Json::Object{
            {"range", toLspRange(diagDocView, diagnostic.location.range)},
            {"severity", lspDiagnosticSeverity(diagnostic)},
            {"code", diagnostic.code},
            {"source", "realscript"},
            {"message", diagnostic.message},
        });
    }
    if (!diags.empty()) {
        publishedNonEmptyDiagnostics_.insert(path);
    } else {
        publishedNonEmptyDiagnostics_.erase(path);
    }
    writeProtocolMessage(output, Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", Json::Object{{"uri", pathToUri(path)}, {"diagnostics", Json(std::move(values))}}},
    });
}

void LspServer::publishWorkspaceDiagnostics(std::ostream& output) {
    (void)service_.build();
    const auto openPaths = service_.openDocumentPaths();
    const std::unordered_set<std::string> openSet(openPaths.begin(), openPaths.end());
    const auto allPaths = service_.documentPaths();
    const std::unordered_set<std::string> allSet(allPaths.begin(), allPaths.end());

    // Clear stale diagnostics on any removed files
    std::vector<std::string> vanished;
    for (const auto& prevPath : publishedNonEmptyDiagnostics_) {
        if (allSet.find(prevPath) == allSet.end()) {
            vanished.push_back(prevPath);
        }
    }
    for (const auto& vanishedPath : vanished) {
        publishedNonEmptyDiagnostics_.erase(vanishedPath);
        writeProtocolMessage(output, Json::Object{
            {"jsonrpc", "2.0"},
            {"method", "textDocument/publishDiagnostics"},
            {"params", Json::Object{{"uri", pathToUri(vanishedPath)}, {"diagnostics", Json::Array{}}}},
        });
    }

    for (const auto& docPath : allPaths) {
        if (openSet.find(docPath) != openSet.end()) {
            publishDiagnostics(docPath, output);
            continue;
        }
        const auto diags = service_.diagnostics(docPath);
        if (!diags.empty() ||
            publishedNonEmptyDiagnostics_.find(docPath) != publishedNonEmptyDiagnostics_.end()) {
            publishDiagnostics(docPath, output);
        }
    }
}

Json LspServer::handle(const Json& message, std::ostream* notifications) {
    const auto* methodValue = message.find("method");
    const auto method = methodValue ? methodValue->stringValue() : std::string{};
    const auto* params = message.find("params");

    if (method == "initialize") {
        positionEncoding_ = PositionEncoding::Utf16;
        if (const auto* encodings = nested(message, {"params", "capabilities", "general", "positionEncodings"});
            encodings && encodings->isArray()) {
            for (const auto& enc : encodings->arrayValue()) {
                if (!enc.isString()) continue;
                if (enc.stringValue() == "utf-8") {
                    positionEncoding_ = PositionEncoding::Utf8;
                    break;
                }
                if (enc.stringValue() == "utf-16") {
                    positionEncoding_ = PositionEncoding::Utf16;
                    break;
                }
            }
        }
        for (const auto& root : extractWorkspaceRoots(params)) {
            (void)service_.addWorkspaceRoot(root);
        }
        if (notifications && !service_.workspaceRoots().empty()) {
            publishWorkspaceDiagnostics(*notifications);
        }
        return response(message, Json::Object{
            {"capabilities", Json::Object{
                {"positionEncoding", positionEncoding_ == PositionEncoding::Utf8 ? "utf-8" : "utf-16"},
                {"textDocumentSync", 2},
                {"workspace", Json::Object{
                    {"workspaceFolders", Json::Object{
                        {"supported", true},
                        {"changeNotifications", true},
                    }},
                }},
                {"completionProvider", Json::Object{
                    {"triggerCharacters", Json::Array{".", "\"", "/"}},
                }},
                {"signatureHelpProvider", Json::Object{
                    {"triggerCharacters", Json::Array{"(", ","}},
                }},
                {"hoverProvider", true},
                {"definitionProvider", true},
                {"referencesProvider", true},
                {"documentSymbolProvider", true},
                {"workspaceSymbolProvider", true},
                {"documentFormattingProvider", true},
                {"codeActionProvider", Json::Object{
                    {"codeActionKinds", Json::Array{"quickfix"}},
                }},
                {"renameProvider", Json::Object{
                    {"prepareProvider", true},
                }},
                {"executeCommandProvider", Json::Object{
                    {"commands", Json::Array{"realscript.refreshManifest"}},
                }},
                {"inlayHintProvider", true},
                {"semanticTokensProvider", Json::Object{
                    {"full", true},
                    {"legend", Json::Object{
                        {"tokenTypes", Json::Array{
                            "namespace", "type", "function", "variable",
                            "parameter", "property", "enumMember", "keyword",
                            "number", "string"}},
                        {"tokenModifiers", Json::Array{"declaration"}},
                    }},
                }},
            }},
            {"serverInfo", Json::Object{{"name", "RealScript LSP"},
                {"version", realscript::kVersionString}}},
        });
    }
    if (method == "initialized" || method == "$/cancelRequest" || method == "$/setTrace") {
        return Json();
    }
    if (method == "shutdown") {
        shutdown_ = true;
        return response(message, nullptr);
    }
    if (method == "exit") return Json();

    if (method == "workspace/didChangeWorkspaceFolders") {
        if (const auto* event = params ? params->find("event") : nullptr;
            event && event->isObject()) {
            if (const auto* removed = event->find("removed"); removed && removed->isArray()) {
                for (const auto& item : removed->arrayValue()) {
                    if (const auto* uriVal = item.find("uri"); uriVal && uriVal->isString()) {
                        service_.removeWorkspaceRoot(uriToPath(uriVal->stringValue()));
                    }
                }
            }
            if (const auto* added = event->find("added"); added && added->isArray()) {
                for (const auto& item : added->arrayValue()) {
                    if (const auto* uriVal = item.find("uri"); uriVal && uriVal->isString()) {
                        (void)service_.addWorkspaceRoot(uriToPath(uriVal->stringValue()));
                    }
                }
            }
            if (notifications) {
                publishWorkspaceDiagnostics(*notifications);
            }
        }
        return Json();
    }

    if (method == "workspace/didChangeWatchedFiles") {
        if (const auto* changes = params ? params->find("changes") : nullptr;
            changes && changes->isArray()) {
            for (const auto& change : changes->arrayValue()) {
                if (!change.isObject()) continue;
                const auto* uriVal = change.find("uri");
                const auto* typeVal = change.find("type");
                if (!uriVal || !uriVal->isString()) continue;
                const std::string filePath = uriToPath(uriVal->stringValue());
                const std::int64_t changeType = typeVal ? typeVal->integerValue(2) : 2;
                if (changeType == 3) {
                    (void)service_.removeDiskFile(filePath);
                } else {
                    (void)service_.syncDiskFile(filePath);
                }
            }
            if (notifications) {
                publishWorkspaceDiagnostics(*notifications);
            }
        }
        return Json();
    }

    if (method == "textDocument/didOpen") {
        const auto* document = params ? params->find("textDocument") : nullptr;
        const auto path = uriToPath(document && document->find("uri")
            ? document->find("uri")->stringValue() : std::string{});
        const auto content = document && document->find("text")
            ? document->find("text")->stringValue() : std::string{};
        const auto version = document && document->find("version")
            ? document->find("version")->integerValue(1) : 1;
        service_.open(path, content, version);
        if (notifications) {
            publishWorkspaceDiagnostics(*notifications);
        }
        return Json();
    }
    if (method == "textDocument/didChange") {
        const auto* document = params ? params->find("textDocument") : nullptr;
        const auto path = uriToPath(document && document->find("uri")
            ? document->find("uri")->stringValue() : std::string{});
        const auto version = document && document->find("version")
            ? document->find("version")->integerValue() : 0;
        std::vector<ContentChange> parsedChanges;
        if (const auto* changes = params ? params->find("contentChanges") : nullptr) {
            const auto& arr = changes->arrayValue();
            std::string workingText = positionEncoding_ == PositionEncoding::Utf16
                ? resolveDocumentText(path)
                : std::string{};
            const auto lineCol1ToByteOffset = [](std::string_view src, std::size_t line1, std::size_t col1) -> std::size_t {
                std::size_t curLine = 1;
                std::size_t idx = 0;
                while (idx < src.size() && curLine < line1) {
                    if (src[idx] == '\n') ++curLine;
                    ++idx;
                }
                std::size_t curCol = 1;
                while (idx < src.size() &&
                       src[idx] != '\n' &&
                       !(src[idx] == '\r' && idx + 1u < src.size() && src[idx + 1u] == '\n') &&
                       curCol < col1) {
                    ++idx;
                    ++curCol;
                }
                return idx;
            };
            for (std::size_t ci = 0; ci < arr.size(); ++ci) {
                const auto& item = arr[ci];
                if (!item.isObject()) continue;
                const auto* text = item.find("text");
                if (!text || !text->isString()) continue;
                ContentChange cc;
                cc.text = text->stringValue();
                if (const auto* rangeObj = item.find("range"); rangeObj && rangeObj->isObject()) {
                    const auto* sObj = rangeObj->find("start");
                    const auto* eObj = rangeObj->find("end");
                    if (sObj && sObj->isObject() && eObj && eObj->isObject()) {
                        const auto startPos = fromLspPosition(workingText, sObj);
                        const auto endPos = fromLspPosition(workingText, eObj);
                        debug::SourceRange sr{};
                        sr.start.line = static_cast<std::uint32_t>(startPos.line + 1);
                        sr.start.column = static_cast<std::uint32_t>(startPos.column + 1);
                        sr.end.line = static_cast<std::uint32_t>(endPos.line + 1);
                        sr.end.column = static_cast<std::uint32_t>(endPos.column + 1);
                        cc.range = sr;
                        if (positionEncoding_ == PositionEncoding::Utf16 && ci + 1u < arr.size()) {
                            const std::size_t sOff = lineCol1ToByteOffset(workingText, sr.start.line, sr.start.column);
                            const std::size_t eOff = std::max(
                                sOff,
                                lineCol1ToByteOffset(workingText, sr.end.line, sr.end.column));
                            workingText.replace(sOff, eOff - sOff, cc.text);
                        }
                    }
                } else if (positionEncoding_ == PositionEncoding::Utf16 && ci + 1u < arr.size()) {
                    workingText = cc.text;
                }
                parsedChanges.push_back(std::move(cc));
            }
        }
        if (!parsedChanges.empty()) {
            service_.applyContentChanges(path, parsedChanges, version);
        } else {
            service_.update(path, std::string{}, version);
        }
        if (notifications) {
            publishWorkspaceDiagnostics(*notifications);
        }
        return Json();
    }
    if (method == "textDocument/didClose") {
        const auto* document = params ? params->find("textDocument") : nullptr;
        const auto path = uriToPath(document && document->find("uri")
            ? document->find("uri")->stringValue() : std::string{});
        service_.close(path);
        if (notifications) {
            if (service_.contains(path)) {
                publishedNonEmptyDiagnostics_.insert(path);
            } else {
                publishedNonEmptyDiagnostics_.erase(path);
                writeProtocolMessage(*notifications, Json::Object{
                    {"jsonrpc", "2.0"},
                    {"method", "textDocument/publishDiagnostics"},
                    {"params", Json::Object{{"uri", pathToUri(path)}, {"diagnostics", Json::Array{}}}},
                });
            }
            publishWorkspaceDiagnostics(*notifications);
        }
        return Json();
    }

    const auto path = uriToPath(nested(message, {"params", "textDocument", "uri"})
        ? nested(message, {"params", "textDocument", "uri"})->stringValue()
        : std::string{});
    const std::string activeDocText =
        (!path.empty() && positionEncoding_ == PositionEncoding::Utf16)
        ? resolveDocumentText(path)
        : std::string{};
    const auto cursor = fromLspPosition(activeDocText, nested(message, {"params", "position"}));

    if (method == "textDocument/completion") {
        Json::Array items;
        for (const auto& item : service_.completion(path, cursor)) {
            items.emplace_back(Json::Object{
                {"label", item.label},
                {"detail", item.detail},
                {"kind", completionKind(item.kind)},
            });
        }
        return response(message, Json(std::move(items)));
    }
    if (method == "textDocument/signatureHelp") {
        const auto help = service_.signatureHelp(path, cursor);
        if (!help) return response(message, nullptr);
        Json::Array sigs;
        for (const auto& sig : help->signatures) {
            Json::Array paramsJson;
            for (const auto& param : sig.parameters) {
                paramsJson.emplace_back(Json::Object{
                    {"label", param.label},
                });
            }
            sigs.emplace_back(Json::Object{
                {"label", sig.label},
                {"documentation", sig.documentation},
                {"parameters", Json(std::move(paramsJson))},
            });
        }
        return response(message, Json::Object{
            {"signatures", Json(std::move(sigs))},
            {"activeSignature", static_cast<std::int64_t>(help->activeSignature)},
            {"activeParameter", static_cast<std::int64_t>(help->activeParameter)},
        });
    }
    if (method == "textDocument/hover") {
        const auto value = service_.hover(path, cursor);
        if (!value) return response(message, nullptr);
        const std::string targetText =
            (positionEncoding_ == PositionEncoding::Utf16 &&
             !value->location.path.empty() &&
             value->location.path != path)
            ? resolveDocumentText(value->location.path)
            : activeDocText;
        return response(message, Json::Object{
            {"contents", Json::Object{{"kind", "plaintext"}, {"value", value->contents}}},
            {"range", toLspRange(targetText, value->location.range)},
        });
    }
    if (method == "textDocument/definition") {
        const auto value = service_.definition(path, cursor);
        return response(message, value ? toLspLocation(*value) : Json(nullptr));
    }
    if (method == "textDocument/references") {
        bool includeDefinition = true;
        if (const auto* include = nested(message, {"params", "context", "includeDeclaration"})) {
            includeDefinition = include->boolValue(true);
        }
        Json::Array values;
        for (const auto& value : service_.references(path, cursor, includeDefinition)) {
            values.push_back(toLspLocation(value));
        }
        return response(message, Json(std::move(values)));
    }
    if (method == "textDocument/documentSymbol") {
        Json::Array values;
        for (const auto& symbol : service_.documentSymbols(path)) {
            values.emplace_back(Json::Object{
                {"name", symbol.name},
                {"detail", symbol.detail},
                {"kind", documentSymbolKind(symbol.kind)},
                {"range", toLspRange(activeDocText, symbol.location.range)},
                {"selectionRange", toLspRange(activeDocText, symbol.location.range)},
            });
        }
        return response(message, Json(std::move(values)));
    }
    if (method == "textDocument/prepareRename") {
        const auto prep = service_.prepareRename(path, cursor);
        if (!prep.valid) {
            return error(
                message,
                -32602,
                prep.errorMessage.empty() ? "symbol cannot be renamed" : prep.errorMessage);
        }
        return response(message, Json::Object{
            {"range", toLspRange(activeDocText, prep.range)},
            {"placeholder", prep.placeholder},
        });
    }
    if (method == "textDocument/rename") {
        const auto* newName = params ? params->find("newName") : nullptr;
        const std::string requestedName = newName ? newName->stringValue() : std::string{};
        const auto prep = service_.prepareRename(path, cursor);
        if (!prep.valid) {
            return error(
                message,
                -32602,
                prep.errorMessage.empty() ? "symbol cannot be renamed" : prep.errorMessage);
        }
        Json::Object changes;
        std::unordered_map<std::string, std::string> docCache;
        for (const auto& edit : service_.rename(path, cursor, requestedName)) {
            std::string_view editDocView = activeDocText;
            if (positionEncoding_ == PositionEncoding::Utf16 && edit.location.path != path) {
                auto [it, inserted] = docCache.try_emplace(edit.location.path);
                if (inserted) it->second = resolveDocumentText(edit.location.path);
                editDocView = it->second;
            }
            auto& array = changes[pathToUri(edit.location.path)].arrayValue();
            array.emplace_back(Json::Object{
                {"range", toLspRange(editDocView, edit.location.range)},
                {"newText", edit.replacement},
            });
        }
        return response(message, Json::Object{{"changes", Json(std::move(changes))}});
    }
    if (method == "textDocument/codeAction") {
        debug::SourceRange reqRange{};
        if (const auto* rangeObj = params ? params->find("range") : nullptr;
            rangeObj && rangeObj->isObject()) {
            reqRange.start = fromLspPosition(activeDocText, rangeObj->find("start"));
            reqRange.end = fromLspPosition(activeDocText, rangeObj->find("end"));
        } else {
            reqRange.start = cursor;
            reqRange.end = cursor;
        }
        Json::Array actionsJson;
        std::unordered_map<std::string, std::string> docCache;
        for (const auto& act : service_.codeActions(path, reqRange)) {
            Json::Object changes;
            for (const auto& edit : act.edits) {
                std::string_view editDocView = activeDocText;
                if (positionEncoding_ == PositionEncoding::Utf16 && edit.location.path != path) {
                    auto [it, inserted] = docCache.try_emplace(edit.location.path);
                    if (inserted) it->second = resolveDocumentText(edit.location.path);
                    editDocView = it->second;
                }
                auto& arr = changes[pathToUri(edit.location.path)].arrayValue();
                arr.emplace_back(Json::Object{
                    {"range", toLspRange(editDocView, edit.location.range)},
                    {"newText", edit.replacement},
                });
            }
            actionsJson.emplace_back(Json::Object{
                {"title", act.title},
                {"kind", act.kind},
                {"isPreferred", act.isPreferred},
                {"edit", Json::Object{{"changes", Json(std::move(changes))}}},
            });
        }
        return response(message, Json(std::move(actionsJson)));
    }
    if (method == "workspace/executeCommand") {
        const std::string cmd = (params && params->find("command"))
            ? params->find("command")->stringValue()
            : std::string{};
        if (cmd == "realscript.refreshManifest") {
            service_.refreshSceneManifest();
            const auto manifest = service_.sceneManifest();
            return response(message, Json::Object{
                {"refreshed", true},
                {"entities", static_cast<std::int64_t>(manifest.entities.size())},
                {"signals", static_cast<std::int64_t>(manifest.signals.size())},
                {"resources", static_cast<std::int64_t>(manifest.resources.size())},
            });
        }
        return error(message, -32601, "unknown command: " + cmd);
    }
    if (method == "textDocument/semanticTokens/full") {
        Json::Array data;
        if (positionEncoding_ == PositionEncoding::Utf8) {
            for (const auto val : service_.semanticTokensEncoded(path)) {
                data.emplace_back(static_cast<std::int64_t>(val));
            }
        } else {
            const auto raw = service_.semanticTokens(path);
            std::size_t prevLine = 0;
            std::size_t prevChar = 0;
            std::size_t cachedLineIdx = static_cast<std::size_t>(-1);
            std::string_view cachedLineView{};
            for (const auto& t : raw) {
                if (t.line != cachedLineIdx) {
                    cachedLineIdx = t.line;
                    cachedLineView = text::lineContentAt(activeDocText, t.line);
                }
                const std::size_t lineLen = cachedLineView.size();
                const std::size_t clampedStart = std::min(t.startChar, lineLen);
                const std::size_t clampedEnd = std::min(t.startChar + t.length, lineLen);
                const std::size_t startU16 = text::utf8ColumnToUtf16Column(cachedLineView, clampedStart);
                const std::size_t endU16 = text::utf8ColumnToUtf16Column(cachedLineView, clampedEnd);
                const std::size_t lenU16 = endU16 >= startU16 ? (endU16 - startU16) : 0u;
                const std::size_t deltaLine = t.line - prevLine;
                const std::size_t deltaStart = (deltaLine == 0) ? (startU16 - prevChar) : startU16;
                data.emplace_back(static_cast<std::int64_t>(deltaLine));
                data.emplace_back(static_cast<std::int64_t>(deltaStart));
                data.emplace_back(static_cast<std::int64_t>(lenU16));
                data.emplace_back(static_cast<std::int64_t>(t.tokenType));
                data.emplace_back(static_cast<std::int64_t>(t.tokenModifiers));
                prevLine = t.line;
                prevChar = startU16;
            }
        }
        return response(message, Json::Object{{"data", Json(std::move(data))}});
    }
    if (method == "textDocument/inlayHint") {
        Json::Array items;
        for (const auto& hint : service_.inlayHints(path)) {
            items.emplace_back(Json::Object{
                {"position", toLspPosition(activeDocText, hint.position)},
                {"label", hint.label},
                {"kind", static_cast<std::int64_t>(hint.kind)},
            });
        }
        return response(message, Json(std::move(items)));
    }
    if (method == "textDocument/formatting") {
        FormatOptions fmtOpts;
        if (const auto* ts = nested(message, {"params", "options", "tabSize"})) {
            fmtOpts.indentWidth = static_cast<std::size_t>(std::max<std::int64_t>(1, ts->integerValue(4)));
        }
        if (const auto* fnl = nested(message, {"params", "options", "insertFinalNewline"})) {
            if (fnl->isBool()) {
                fmtOpts.insertFinalNewline = fnl->boolValue(true);
            }
        }
        Json::Array edits;
        for (const auto& edit : service_.formatDocument(path, fmtOpts)) {
            edits.emplace_back(Json::Object{
                {"range", toLspRange(activeDocText, edit.location.range)},
                {"newText", edit.replacement},
            });
        }
        return response(message, Json(std::move(edits)));
    }
    if (method == "workspace/symbol") {
        const auto query = (params && params->find("query"))
            ? params->find("query")->stringValue()
            : std::string{};
        Json::Array values;
        for (const auto& symbol : service_.workspaceSymbols(query)) {
            values.emplace_back(Json::Object{
                {"name", symbol.name},
                {"kind", documentSymbolKind(symbol.kind)},
                {"location", toLspLocation(symbol.location)},
            });
        }
        return response(message, Json(std::move(values)));
    }

    if (message.find("id")) return error(message, -32601, "method not found: " + method);
    return Json();
}

int LspServer::run(std::istream& input, std::ostream& output) {
    std::string body;
    while (readProtocolMessage(input, body)) {
        std::string parseError;
        auto message = Json::parse(body, parseError);
        if (!message) {
            writeProtocolMessage(output, Json::Object{
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", Json::Object{{"code", -32700}, {"message", parseError}}},
            });
            continue;
        }
        const auto result = handle(*message, &output);
        if (message->find("id")) writeProtocolMessage(output, result);
        const auto* method = message->find("method");
        if (method && method->stringValue() == "exit") break;
    }
    return shutdown_ ? 0 : 1;
}

} // namespace realscript::tooling
