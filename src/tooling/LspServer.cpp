#include "realscript/tooling/LspServer.h"

#include <algorithm>
#include <string>

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

Json location(const Location& value) {
    return Json::Object{{"uri", pathToUri(value.path)}, {"range", range(value.range)}};
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

void LspServer::publishDiagnostics(const std::string& path, std::ostream& output) {
    Json::Array values;
    for (const auto& diagnostic : service_.diagnostics(path)) {
        values.emplace_back(Json::Object{
            {"range", range(diagnostic.location.range)},
            {"severity", diagnostic.severity == diagnostics::DiagnosticSeverity::Error ? 1 : 2},
            {"code", diagnostic.code},
            {"source", "realscript"},
            {"message", diagnostic.message},
        });
    }
    writeProtocolMessage(output, Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", Json::Object{{"uri", pathToUri(path)}, {"diagnostics", Json(std::move(values))}}},
    });
}

Json LspServer::handle(const Json& message, std::ostream* notifications) {
    const auto* methodValue = message.find("method");
    const auto method = methodValue ? methodValue->stringValue() : std::string{};
    const auto* params = message.find("params");

    if (method == "initialize") {
        return response(message, Json::Object{
            {"capabilities", Json::Object{
                {"textDocumentSync", 1},
                {"completionProvider", Json::Object{}},
                {"hoverProvider", true},
                {"definitionProvider", true},
                {"referencesProvider", true},
                {"documentSymbolProvider", true},
                {"renameProvider", true},
            }},
            {"serverInfo", Json::Object{{"name", "RealScript LSP"}, {"version", "0.1"}}},
        });
    }
    if (method == "shutdown") {
        shutdown_ = true;
        return response(message, nullptr);
    }
    if (method == "exit") return Json();

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
            for (const auto& documentPath : service_.documentPaths()) {
                publishDiagnostics(documentPath, *notifications);
            }
        }
        return Json();
    }
    if (method == "textDocument/didChange") {
        const auto* document = params ? params->find("textDocument") : nullptr;
        const auto path = uriToPath(document && document->find("uri")
            ? document->find("uri")->stringValue() : std::string{});
        const auto version = document && document->find("version")
            ? document->find("version")->integerValue() : 0;
        std::string content;
        if (const auto* changes = params ? params->find("contentChanges") : nullptr) {
            if (!changes->arrayValue().empty()) {
                const auto* text = changes->arrayValue().front().find("text");
                if (text) content = text->stringValue();
            }
        }
        service_.update(path, content, version);
        if (notifications) {
            for (const auto& documentPath : service_.documentPaths()) {
                publishDiagnostics(documentPath, *notifications);
            }
        }
        return Json();
    }
    if (method == "textDocument/didClose") {
        const auto* document = params ? params->find("textDocument") : nullptr;
        const auto path = uriToPath(document && document->find("uri")
            ? document->find("uri")->stringValue() : std::string{});
        service_.close(path);
        if (notifications) {
            writeProtocolMessage(*notifications, Json::Object{
                {"jsonrpc", "2.0"},
                {"method", "textDocument/publishDiagnostics"},
                {"params", Json::Object{{"uri", pathToUri(path)}, {"diagnostics", Json::Array{}}}},
            });
            for (const auto& documentPath : service_.documentPaths()) {
                publishDiagnostics(documentPath, *notifications);
            }
        }
        return Json();
    }

    const auto path = uriToPath(nested(message, {"params", "textDocument", "uri"})
        ? nested(message, {"params", "textDocument", "uri"})->stringValue()
        : std::string{});
    const auto cursor = readPosition(nested(message, {"params", "position"}));

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
    if (method == "textDocument/hover") {
        const auto value = service_.hover(path, cursor);
        if (!value) return response(message, nullptr);
        return response(message, Json::Object{
            {"contents", Json::Object{{"kind", "plaintext"}, {"value", value->contents}}},
            {"range", range(value->location.range)},
        });
    }
    if (method == "textDocument/definition") {
        const auto value = service_.definition(path, cursor);
        return response(message, value ? location(*value) : Json(nullptr));
    }
    if (method == "textDocument/references") {
        bool includeDefinition = true;
        if (const auto* include = nested(message, {"params", "context", "includeDeclaration"})) {
            includeDefinition = include->boolValue(true);
        }
        Json::Array values;
        for (const auto& value : service_.references(path, cursor, includeDefinition)) {
            values.push_back(location(value));
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
                {"range", range(symbol.location.range)},
                {"selectionRange", range(symbol.location.range)},
            });
        }
        return response(message, Json(std::move(values)));
    }
    if (method == "textDocument/rename") {
        const auto* newName = params ? params->find("newName") : nullptr;
        Json::Object changes;
        for (const auto& edit : service_.rename(
                 path, cursor, newName ? newName->stringValue() : std::string{})) {
            auto& array = changes[pathToUri(edit.location.path)].arrayValue();
            array.emplace_back(Json::Object{
                {"range", range(edit.location.range)},
                {"newText", edit.replacement},
            });
        }
        return response(message, Json::Object{{"changes", Json(std::move(changes))}});
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
