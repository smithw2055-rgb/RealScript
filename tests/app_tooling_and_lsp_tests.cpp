#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ProjectConfig.h"
#include "realscript/text/AssetGuid.h"
#include "realscript/text/Text.h"
#include "realscript/tooling/Formatter.h"
#include "realscript/tooling/GovernanceFuzzer.h"
#include "realscript/tooling/GovernanceLinter.h"
#include "realscript/tooling/Json.h"
#include "realscript/tooling/LanguageService.h"
#include "realscript/tooling/LspServer.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace realscript;

[[noreturn]] void failCheck(const char* expr, int line) noexcept {
    std::cerr << "Check failed at app_tooling_and_lsp_tests.cpp:" << line << ": " << expr << std::endl;
    std::exit(1);
}

#define RS_CHECK(...)                                                                              \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            failCheck(#__VA_ARGS__, __LINE__);                                                     \
        }                                                                                          \
    } while (false)

struct TempDirGuard {
    std::filesystem::path dir;
    explicit TempDirGuard(std::string prefix) {
        dir = std::filesystem::temp_directory_path() / (prefix + "_app_tooling");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
};

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    RS_CHECK(out.is_open());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    RS_CHECK(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int runProcess(const std::string& commandLine) {
#ifdef _WIN32
    const std::string wrapped = "\"" + commandLine + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(commandLine.c_str());
#endif
}

std::string buildLspMessage(const tooling::Json& json) {
    const std::string body = json.dump();
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::vector<tooling::Json> parseLspMessages(const std::string& streamText) {
    std::vector<tooling::Json> out;
    std::size_t pos = 0;
    while (pos < streamText.size()) {
        const auto headerEnd = streamText.find("\r\n\r\n", pos);
        if (headerEnd == std::string::npos) break;
        const std::string headers = streamText.substr(pos, headerEnd - pos);
        const auto clPos = headers.find("Content-Length:");
        RS_CHECK(clPos != std::string::npos);
        const auto lineEnd = headers.find('\n', clPos);
        const std::string lenStr = headers.substr(
            clPos + 15,
            lineEnd == std::string::npos ? std::string::npos : lineEnd - (clPos + 15));
        const auto len = static_cast<std::size_t>(std::stoul(lenStr));
        const std::size_t bodyStart = headerEnd + 4;
        RS_CHECK(bodyStart + len <= streamText.size());
        std::string parseErr;
        const auto parsed = tooling::Json::parse(streamText.substr(bodyStart, len), parseErr);
        RS_CHECK(parsed.has_value());
        out.push_back(*parsed);
        pos = bodyStart + len;
    }
    return out;
}

void testFormatterIdempotencyUiStringsAndDirectives() {
    const std::string messy =
        "#nullable enable\n"
        "#pragma warning disable RSG003\n"
        "#region Generated View\n"
        "module   Tooling.UiTest;\n"
        "\n"
        "public   class   Panel\n"
        "{\n"
        "    public string render(string title)\n"
        "    {\n"
        "        string markup =   ui@\"<div id='hdr'>{title}</div>\"  ;\n"
        "        string query=sql@\"SELECT name FROM hud\";\n"
        "#line 120 \"gen/panel.rscript\"\n"
        "        if(title==\"ok\"){return markup;}\n"
        "#line hidden\n"
        "        string dbg = query;\n"
        "#line default\n"
        "        return markup;\n"
        "    }\n"
        "}\n"
        "#endregion\n";

    const auto r1 = tooling::formatSource(messy);
    RS_CHECK(r1.changed);
    RS_CHECK(!r1.unchangedOnError);

    // Tagged verbatim strings (`ui@"..."`, `sql@"..."`) MUST remain flush without spaces.
    RS_CHECK(r1.formatted.find("ui@\"<div") != std::string::npos);
    RS_CHECK(r1.formatted.find("ui @\"") == std::string::npos);
    RS_CHECK(r1.formatted.find("sql@\"SELECT") != std::string::npos);
    RS_CHECK(r1.formatted.find("sql @\"") == std::string::npos);

    // All preprocessor directives (#nullable, #pragma, #region, #endregion, #line) must survive intact.
    RS_CHECK(r1.formatted.find("#nullable enable") != std::string::npos);
    RS_CHECK(r1.formatted.find("#pragma warning disable RSG003") != std::string::npos);
    RS_CHECK(r1.formatted.find("#region Generated View") != std::string::npos);
    RS_CHECK(r1.formatted.find("#line 120 \"gen/panel.rscript\"") != std::string::npos);
    RS_CHECK(r1.formatted.find("#line hidden") != std::string::npos);
    RS_CHECK(r1.formatted.find("#line default") != std::string::npos);
    RS_CHECK(r1.formatted.find("#endregion") != std::string::npos);

    // Formatting is strictly idempotent on pass 2.
    const auto r2 = tooling::formatSource(r1.formatted);
    RS_CHECK(!r2.changed);
    RS_CHECK(r2.formatted == r1.formatted);

    // Semantic equivalence before and after formatting on valid RealScript code.
    const std::string validScript =
        "#region Math\n"
        "module Tooling.FmtCheck;\n"
        "public int calc(int a,int b){if(a>b){return a+b;}return b-a;}\n"
        "#endregion\n";
    const auto fmtValid = tooling::formatSource(validScript);
    RS_CHECK(fmtValid.changed);
    RS_CHECK(fmtValid.formatted.find("#region Math") != std::string::npos);
    RS_CHECK(fmtValid.formatted.find("#endregion") != std::string::npos);

    compiler::Compilation beforeComp({{"fmt.rscript", validScript}});
    compiler::Compilation afterComp({{"fmt.rscript", fmtValid.formatted}});
    const auto beforeBuild = beforeComp.build();
    const auto afterBuild = afterComp.build();
    RS_CHECK(!beforeBuild.diagnostics.hasErrors());
    RS_CHECK(!afterBuild.diagnostics.hasErrors());
    bytecode::Lowerer lowerer;
    const auto encBefore = bytecode::encodeModule(lowerer.lower(beforeBuild.modules.front()));
    const auto encAfter = bytecode::encodeModule(lowerer.lower(afterBuild.modules.front()));
    RS_CHECK(!encBefore.empty() && !encAfter.empty());

    // Custom indentWidth & maxConsecutiveBlankLines options.
    tooling::FormatOptions customOpts;
    customOpts.indentWidth = 2;
    customOpts.maxConsecutiveBlankLines = 0;
    const auto rCustom = tooling::formatSource(validScript, customOpts);
    RS_CHECK(!rCustom.unchangedOnError);
    RS_CHECK(!tooling::formatSource(rCustom.formatted, customOpts).changed);

    // Lexer error (e.g. unterminated string literal) must safely return unchangedOnError = true.
    const std::string broken = "module Tooling.Broken;\nstring s = \"unterminated;\n";
    const auto rBroken = tooling::formatSource(broken);
    RS_CHECK(rBroken.unchangedOnError);
    RS_CHECK(rBroken.formatted == broken);

    std::cout << "[PASS] Formatter idempotency, ui@\"...\" tagged strings & directives\n";
}

void testGovernanceLinterRulesAndSuppressions() {
    const std::string source =
        "module Tooling.LintCheck;\n"
        "\n"
        "public class Box {\n"
        "    public int value = 1;\n"
        "}\n"
        "\n"
        "public void OnUpdate()\n"
        "{\n"
        "    var a = new Box();\n"
        "    // realscript-disable-next-line RSG001\n"
        "    var b = new Box();\n"
        "    int unusedLocal = a.value + b.value;\n"
        "    int suppressedUnused = 7; // realscript-disable-line RSG003\n"
        "}\n"
        "\n"
        "public int complexFn(int n)\n"
        "{\n"
        "    int acc = 0;\n"
        "    if (n > 1) { acc = acc + 1; }\n"
        "    if (n > 2) { acc = acc + 2; }\n"
        "    if (n > 3) { acc = acc + 3; }\n"
        "    if (n > 4) { acc = acc + 4; }\n"
        "    return acc;\n"
        "}\n"
        "\n"
        "public int suspiciousFn(int x)\n"
        "{\n"
        "    if (x == 5) {\n"
        "        return 10;\n"
        "    } else {\n"
        "        return 10;\n"
        "    }\n"
        "}\n";

    tooling::LintConfig cfg;
    cfg.enableGovernanceLint = true;
    cfg.maxCyclomaticComplexity = 3;

    tooling::GovernanceLinter linter(cfg);
    const auto diags = linter.lintSource("lint.rscript", source);

    bool sawRsg001 = false;
    std::size_t rsg001Count = 0;
    bool sawRsg002 = false;
    bool sawRsg003 = false;
    bool sawSuppressedLocal = false;
    bool sawRsg005 = false;
    for (const auto& d : diags) {
        if (d.code == "RSG001") {
            sawRsg001 = true;
            ++rsg001Count;
        } else if (d.code == "RSG002") {
            sawRsg002 = true;
        } else if (d.code == "RSG003") {
            sawRsg003 = true;
            if (d.message.find("suppressedUnused") != std::string::npos) {
                sawSuppressedLocal = true;
            }
        } else if (d.code == "RSG005") {
            sawRsg005 = true;
        }
    }
    RS_CHECK(sawRsg001);
    RS_CHECK(rsg001Count == 1); // Second `new Box()` is suppressed by `realscript-disable-next-line RSG001`
    RS_CHECK(sawRsg002);
    RS_CHECK(sawRsg003);
    RS_CHECK(!sawSuppressedLocal); // `realscript-disable-line RSG003` works
    RS_CHECK(sawRsg005);

    // JSON project config override: turn RSG001 to "error", RSG002 to "off".
    std::string cfgErr;
    const auto jsonCfg = tooling::Json::parse(R"({
        "lint": {
            "enabled": true,
            "maxCyclomaticComplexity": 3,
            "rules": {
                "hot-path-allocation": "error",
                "RSG002": "off"
            }
        }
    })", cfgErr);
    RS_CHECK(jsonCfg.has_value());
    tooling::LintConfig parsedCfg;
    RS_CHECK(tooling::parseLintConfigFromProjectJson(*jsonCfg, parsedCfg));
    RS_CHECK(parsedCfg.severityFor("RSG001") == tooling::LintSeverity::Error);
    RS_CHECK(parsedCfg.severityFor("RSG002") == tooling::LintSeverity::Off);

    tooling::GovernanceLinter customLinter(parsedCfg);
    const auto customDiags = customLinter.lintSource("lint.rscript", source);
    for (const auto& d : customDiags) {
        RS_CHECK(d.code != "RSG002");
        if (d.code == "RSG001") {
            RS_CHECK(d.severity == tooling::LintSeverity::Error);
        }
    }

    std::cout << "[PASS] GovernanceLinter rules, config & inline suppressions\n";
}

void testLanguageServiceNavigationUtf16AndSceneGuids() {
    // Verify text::AssetGuid canonical derivation & parsing cleanly without GameSdk.
    const auto typeGuid = text::AssetGuid::fromTypeName("Tooling.Nav::Player");
    const auto assetGuid = text::AssetGuid::fromAssetPath("res://Prefabs/Hero.prefab");
    RS_CHECK(!typeGuid.toString().empty());
    RS_CHECK(!assetGuid.toString().empty());
    RS_CHECK(text::AssetGuid::parse(assetGuid.toString()).has_value());
    RS_CHECK(text::AssetGuid::normalizeAssetPath("./Prefabs\\Sub//Hero.prefab/") ==
           "Prefabs/Sub/Hero.prefab");
    RS_CHECK(text::AssetGuid::fromAssetPath("Prefabs\\Hero.prefab") ==
           text::AssetGuid::fromAssetPath("./Prefabs//Hero.prefab/"));

    tooling::LanguageService svc;
    const std::string projectConfig = R"RS({
        "entities": [
            { "name": "HeroNode", "path": "/Root/HeroNode", "scriptType": "Tooling.Nav::Player" }
        ],
        "signals": [
            { "name": "onScored", "ownerType": "Tooling.Nav::Player", "signature": "void onScored(int pts)" }
        ],
        "resources": [
            { "path": "res://Prefabs/Hero.prefab", "type": "Prefab" }
        ]
    })RS";
    RS_CHECK(svc.loadProjectConfigJson(projectConfig));
    const auto manifest = svc.sceneManifest();
    RS_CHECK(!manifest.resources.empty());
    RS_CHECK(manifest.resources[0].guid == assetGuid.toString());

    const std::string navSource =
        "module Tooling.Nav;\n"
        "public class Player {\n"
        "    public int score = 10;\n"
        "    public string badge() { return \"你好🚀\"; }\n"
        "    public int addScore(int delta)\n"
        "    {\n"
        "        // Reference score after CJK + UTF-16 surrogate pair emoji.\n"
        "        string prefix = \"你好🚀\"; int next = this.score + delta;\n"
        "        return next;\n"
        "    }\n"
        "}\n";
    svc.open("player.rscript", navSource, 1);

    const auto diags = svc.diagnostics("player.rscript");
    RS_CHECK(diags.empty());

    // On line 7 (`        string prefix = "你好🚀"; int next = this.score + delta;`),
    // locate `score` in UTF-8 byte coordinates for LanguageService direct calls.
    const std::string targetLine =
        "        string prefix = \"你好🚀\"; int next = this.score + delta;";
    const auto scoreColUtf8 = targetLine.find("score");
    RS_CHECK(scoreColUtf8 != std::string::npos);
    const auto scoreCursor = text::LinePosition{7u, static_cast<std::uint32_t>(scoreColUtf8 + 1)};

    const auto def = svc.definition("player.rscript", scoreCursor);
    RS_CHECK(def.has_value());
    RS_CHECK(def->range.start.line == 2u);

    const auto hover = svc.hover("player.rscript", scoreCursor);
    RS_CHECK(hover.has_value());
    RS_CHECK(hover->contents.find("int") != std::string::npos);

    const auto refs = svc.references("player.rscript", text::LinePosition{2u, 15u}, true);
    RS_CHECK(refs.size() >= 2);

    const auto prep = svc.prepareRename("player.rscript", scoreCursor);
    RS_CHECK(prep.valid);
    const auto edits = svc.rename("player.rscript", scoreCursor, "points");
    RS_CHECK(edits.size() >= 2);

    // Completions on `this.`
    const auto dotCol = targetLine.find("this.") + 5;
    const auto comps = svc.completion("player.rscript", text::LinePosition{7u, static_cast<std::uint32_t>(dotCol)});
    bool foundScoreComp = false;
    for (const auto& c : comps) {
        if (c.label == "score") {
            foundScoreComp = true;
            break;
        }
    }
    RS_CHECK(foundScoreComp);

    const auto tokens = svc.semanticTokens("player.rscript");
    RS_CHECK(!tokens.empty());
    const auto hints = svc.inlayHints("player.rscript");
    (void)hints;

    // Incremental document update via applyContentChanges (1-based line & column).
    RS_CHECK(svc.applyContentChanges(
        "player.rscript",
        {tooling::ContentChange{
            std::optional<debug::SourceRange>{debug::SourceRange{
                0u,
                text::TextSpan{},
                text::LinePosition{3u, 16u},
                text::LinePosition{3u, 21u},
            }},
            "scoreVal",
        }},
        2));
    RS_CHECK(svc.documentContent("player.rscript").find("scoreVal = 10;") != std::string::npos);

    // Code actions on a range cleanly returns vector without crash.
    const auto actions = svc.codeActions(
        "player.rscript",
        debug::SourceRange{0u, text::TextSpan{}, text::LinePosition{1u, 1u}, text::LinePosition{1u, 10u}});
    (void)actions;

    std::cout << "[PASS] LanguageService navigation, UTF-16 & scene-manifest AssetGuid\n";
}

void testLspServerFramingAndUtf16RoundTrip() {
    // Line 3: `        string tag = "你好🚀"; int val = compute(1);`
    // "你好" = 2 UTF-16 units (6 UTF-8 bytes), "🚀" = 2 UTF-16 units (4 UTF-8 bytes).
    // Verify LSP UTF-16 column positions resolve `compute` accurately!
    const std::string lspScript =
        "module Tooling.Lsp;\n"
        "public int compute(int x) { return x * 2; }\n"
        "public int entry()\n"
        "{\n"
        "    string tag = \"你好🚀\"; int res = compute(21);\n"
        "    return res;\n"
        "}\n";

    // Calculate UTF-16 code-unit column of `compute` on line 4 (`    string tag = "你好🚀"; int res = compute(21);`).
    // ASCII prefix `    string tag = "` = 18 units
    // "你好" = 2 units
    // "🚀" = 2 units
    // ASCII suffix `"; int res = ` = 13 units -> total = 35 UTF-16 units!
    const std::string line4Prefix = "    string tag = \"";
    const std::string line4Suffix = "\"; int res = ";
    const std::int64_t computeUtf16Col = static_cast<std::int64_t>(
        line4Prefix.size() + 2 + 2 + line4Suffix.size());

    std::string inputStream;
    const std::string uri = "file:///lsp_utf16.rscript";
    inputStream += buildLspMessage(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"id", std::int64_t{1}},
        {"method", "initialize"},
        {"params", tooling::Json::Object{
            {"capabilities", tooling::Json::Object{
                {"general", tooling::Json::Object{
                    {"positionEncodings", tooling::Json::Array{tooling::Json("utf-16")}},
                }},
            }},
        }},
    });
    inputStream += buildLspMessage(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "initialized"},
        {"params", tooling::Json::Object{}},
    });
    inputStream += buildLspMessage(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{
                {"uri", uri},
                {"languageId", "realscript"},
                {"version", std::int64_t{1}},
                {"text", lspScript},
            }},
        }},
    });
    inputStream += buildLspMessage(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"id", std::int64_t{2}},
        {"method", "textDocument/definition"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{{"uri", uri}}},
            {"position", tooling::Json::Object{
                {"line", std::int64_t{4}},
                {"character", computeUtf16Col},
            }},
        }},
    });
    inputStream += buildLspMessage(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"id", std::int64_t{3}},
        {"method", "textDocument/hover"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{{"uri", uri}}},
            {"position", tooling::Json::Object{
                {"line", std::int64_t{4}},
                {"character", computeUtf16Col},
            }},
        }},
    });
    inputStream += buildLspMessage(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"id", std::int64_t{4}},
        {"method", "textDocument/formatting"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{{"uri", uri}}},
            {"options", tooling::Json::Object{{"tabSize", std::int64_t{4}}}},
        }},
    });
    inputStream += buildLspMessage(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"id", std::int64_t{5}},
        {"method", "shutdown"},
        {"params", tooling::Json::Object{}},
    });
    inputStream += buildLspMessage(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "exit"},
    });

    std::istringstream in(inputStream);
    std::ostringstream out;
    tooling::LspServer server;
    const int exitCode = server.run(in, out);
    RS_CHECK(exitCode == 0);
    RS_CHECK(server.positionEncoding() == tooling::LspServer::PositionEncoding::Utf16);

    const auto responses = parseLspMessages(out.str());
    bool checkedDef = false;
    bool checkedHover = false;
    bool checkedFmt = false;
    for (const auto& msg : responses) {
        const auto* idNode = msg.find("id");
        if (!idNode) continue;
        const auto id = idNode->integerValue(0);
        if (id == 2) {
            const auto* res = msg.find("result");
            RS_CHECK(res && res->isObject());
            const auto line = res->find("range")->find("start")->find("line")->integerValue(-1);
            RS_CHECK(line == 1); // Line 1 is `public int compute(int x)`
            checkedDef = true;
        } else if (id == 3) {
            const auto* res = msg.find("result");
            RS_CHECK(res && res->isObject());
            RS_CHECK(res->dump().find("compute") != std::string::npos);
            checkedHover = true;
        } else if (id == 4) {
            const auto* res = msg.find("result");
            RS_CHECK(res && res->isArray());
            checkedFmt = true;
        }
    }
    RS_CHECK(checkedDef && checkedHover && checkedFmt);

    std::cout << "[PASS] LspServer JSON-RPC framing & UTF-16 CJK+Emoji offsets\n";
}

void testGovernanceFuzzerDifferentialAndMutation() {
    tooling::DifferentialFuzzOptions diffOpts;
    diffOpts.seed = 0x12345678ULL;
    diffOpts.programCount = 8;
    diffOpts.inputsPerFunction = 4;
    const auto diffReport = tooling::runDifferentialFuzz(diffOpts);
    RS_CHECK(diffReport.succeeded());
    RS_CHECK(diffReport.programsGenerated == 8);
    RS_CHECK(diffReport.invocationsExecuted > 0);

    tooling::BytecodeMutationFuzzOptions mutOpts;
    mutOpts.seed = 0xCAFEBABEULL;
    mutOpts.mutationsPerSeedModule = 64;
    const auto mutReport = tooling::runBytecodeMutationFuzz(mutOpts);
    RS_CHECK(mutReport.succeeded());
    RS_CHECK(mutReport.totalMutations > 0);

    std::cout << "[PASS] GovernanceFuzzer differential & bytecode mutation suites\n";
}

void testRscCliFormatAndLintSmoke() {
#ifdef REALSCRIPT_RSC_PATH
    const std::string rscExe = REALSCRIPT_RSC_PATH;
    if (rscExe.empty()) {
        std::cout << "[SKIP] REALSCRIPT_RSC_PATH empty\n";
        return;
    }

    TempDirGuard guard("rs_cli_smoke");
    const auto fmtFile = guard.dir / "cli_format.rscript";
    const auto lintFile = guard.dir / "cli_lint.rscript";

    const std::string unformatted =
        "#region Cli\n"
        "module   Tooling.Cli;\n"
        "public string badge(){return ui@\"<span>ok</span>\";}\n"
        "#endregion\n";
    writeTextFile(fmtFile, unformatted);

    const auto fmtLog = guard.dir / "fmt_out.txt";
    // Unformatted file fails `--check-format`.
    const int checkBefore = runProcess(
        "\"" + rscExe + "\" \"" + fmtFile.string() + "\" --check-format > \"" +
        fmtLog.string() + "\" 2>&1");
    RS_CHECK(checkBefore != 0);

    // `--format --write` formats cleanly and preserves `ui@"..."` + `#region`/`#endregion`.
    const int fmtRc = runProcess(
        "\"" + rscExe + "\" \"" + fmtFile.string() + "\" --format --write");
    RS_CHECK(fmtRc == 0);
    const std::string afterFmt = readTextFile(fmtFile);
    RS_CHECK(afterFmt.find("ui@\"<span>ok</span>\"") != std::string::npos);
    RS_CHECK(afterFmt.find("ui @\"") == std::string::npos);
    RS_CHECK(afterFmt.find("#region Cli") != std::string::npos);
    RS_CHECK(afterFmt.find("#endregion") != std::string::npos);

    // Formatted file passes `--check-format`.
    const int checkAfter = runProcess(
        "\"" + rscExe + "\" \"" + fmtFile.string() + "\" --check-format");
    RS_CHECK(checkAfter == 0);

    // `--lint` detects warning and `--lint-deny-warnings` returns non-zero.
    const std::string lintScript =
        "module Tooling.CliLint;\n"
        "public class Item { public int x = 1; }\n"
        "public void OnUpdate() { var a = new Item(); }\n";
    writeTextFile(lintFile, lintScript);
    const auto lintLog = guard.dir / "lint_out.txt";
    const int lintWarnRc = runProcess(
        "\"" + rscExe + "\" \"" + lintFile.string() + "\" --lint > \"" + lintLog.string() + "\" 2>&1");
    RS_CHECK(lintWarnRc == 0);
    RS_CHECK(readTextFile(lintLog).find("RSG001") != std::string::npos);

    const int lintDenyRc = runProcess(
        "\"" + rscExe + "\" \"" + lintFile.string() + "\" --lint --lint-deny-warnings > \"" +
        lintLog.string() + "\" 2>&1");
    RS_CHECK(lintDenyRc != 0);

    std::cout << "[PASS] rsc CLI --format, --check-format, --lint & --lint-deny-warnings\n";
#else
#error "app_tooling_and_lsp_tests requires REALSCRIPT_RSC_PATH compile definition"
#endif
}

std::string jsonEscapeLspPath(const std::filesystem::path& p) {
    const std::string raw = p.generic_string();
    std::string out;
    out.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

void testProjectConfigMacroPluginsAndSkeletonFastpath() {
#ifdef REALSCRIPT_TEST_MACRO_PLUGIN_PATH
    const std::filesystem::path pluginPath = REALSCRIPT_TEST_MACRO_PLUGIN_PATH;
    RS_CHECK(std::filesystem::exists(pluginPath));

    TempDirGuard guard("rs_lsp_macro");
    const auto cfgPath = guard.dir / "realscript.project.json";
    writeTextFile(cfgPath,
        "{\n"
        "  \"macroPlugins\": [ \"" + jsonEscapeLspPath(pluginPath) + "\" ]\n"
        "}\n");

    const std::string actorSrc =
        "module Tooling.MacroLsp;\n"
        "\n"
        "[AutoGetter(\"health\")]\n"
        "public partial class Actor {\n"
        "    public int health = 50;\n"
        "    public int Compute() {\n"
        "        int x = this.GetHealth() + plugin_magic(10);\n"
        "        return x;\n"
        "    }\n"
        "}\n";
    const auto actorPathFs = guard.dir / "src" / "Actor.rscript";
    writeTextFile(actorPathFs, actorSrc);
    const std::string actorPath = actorPathFs.generic_string();

    // 1. LanguageService via loadProjectConfig(dir)
    {
        tooling::LanguageService svc;
        diagnostics::DiagnosticBag cfgDiag;
        RS_CHECK(svc.loadProjectConfig(guard.dir, &cfgDiag));
        RS_CHECK(!cfgDiag.hasErrors());

        svc.open(actorPath, actorSrc, 1);
        const auto diags = svc.diagnostics(actorPath);
        RS_CHECK(diags.empty());

        const std::string dotLine = "        int x = this.GetHealth() + plugin_magic(10);";
        const auto dotCol = dotLine.find("this.") + 5;
        const auto comps = svc.completion(actorPath, text::LinePosition{6u, static_cast<std::uint32_t>(dotCol)});
        bool hasGetHealth = false;
        bool hasGetAutoValue = false;
        for (const auto& c : comps) {
            if (c.label == "GetHealth") hasGetHealth = true;
            if (c.label == "GetAutoValue") hasGetAutoValue = true;
        }
        RS_CHECK(hasGetHealth && hasGetAutoValue);
    }

    // 2. LanguageService via explicit setCompilationOptions(CompilationOptions)
    {
        tooling::LanguageService manualSvc;
        diagnostics::DiagnosticBag pluginDiag;
        auto plugin = compiler::loadNativeCompilerMacroPlugin(pluginPath, &pluginDiag);
        RS_CHECK(plugin != nullptr && !pluginDiag.hasErrors());

        compiler::CompilationOptions opts;
        opts.macroPlugins.push_back(std::move(plugin));
        manualSvc.setCompilationOptions(std::move(opts));

        manualSvc.open("ActorManual.rscript", actorSrc, 1);
        RS_CHECK(manualSvc.diagnostics("ActorManual.rscript").empty());

        const std::string dotLine = "        int x = this.GetHealth() + plugin_magic(10);";
        const auto dotCol = dotLine.find("this.") + 5;
        const auto comps = manualSvc.completion(
            "ActorManual.rscript", text::LinePosition{6u, static_cast<std::uint32_t>(dotCol)});
        bool hasGetHealth = false;
        for (const auto& c : comps) {
            if (c.label == "GetHealth") {
                hasGetHealth = true;
                break;
            }
        }
        RS_CHECK(hasGetHealth);
    }
#endif

    // 3. Declaration-skeleton fastpath: body-only edit increments peerRevalidationSkips(); signature edit does not.
    {
        tooling::LanguageService svcSkel;
        const std::string peerV1 =
            "module Tooling.SkelPeer;\n"
            "public class Helper {\n"
            "    public int Double(int v) {\n"
            "        return v * 2;\n"
            "    }\n"
            "}\n";
        const std::string mainSrc =
            "module Tooling.SkelMain;\n"
            "import Tooling.SkelPeer;\n"
            "public class Caller {\n"
            "    public int Run(Helper h) {\n"
            "        return h.Double(4);\n"
            "    }\n"
            "}\n";

        svcSkel.open("skel_peer.rscript", peerV1, 1);
        svcSkel.open("skel_main.rscript", mainSrc, 1);
        RS_CHECK(svcSkel.diagnostics("skel_peer.rscript").empty());
        RS_CHECK(svcSkel.diagnostics("skel_main.rscript").empty());
        const std::size_t skipsInitial = svcSkel.peerRevalidationSkips();

        const std::string peerBodyEdit =
            "module Tooling.SkelPeer;\n"
            "public class Helper {\n"
            "    public int Double(int v) {\n"
            "        return v * 3 + 1;\n"
            "    }\n"
            "}\n";
        svcSkel.update("skel_peer.rscript", peerBodyEdit, 2);
        RS_CHECK(svcSkel.diagnostics("skel_peer.rscript").empty());
        const std::size_t skipsAfterBody = svcSkel.peerRevalidationSkips();
        RS_CHECK(skipsAfterBody > skipsInitial);

        const std::string peerSigEdit =
            "module Tooling.SkelPeer;\n"
            "public class Helper {\n"
            "    public int Double(int v) {\n"
            "        return v * 3 + 1;\n"
            "    }\n"
            "    public int Extra(int k) {\n"
            "        return k;\n"
            "    }\n"
            "}\n";
        svcSkel.update("skel_peer.rscript", peerSigEdit, 3);
        RS_CHECK(svcSkel.diagnostics("skel_peer.rscript").empty());
        const std::size_t skipsAfterSig = svcSkel.peerRevalidationSkips();
        RS_CHECK(skipsAfterSig == skipsAfterBody);
    }

    std::cout << "[PASS] Pillar 5: LanguageService project config, C11 macros & declaration-skeleton fastpath\n";
}

} // namespace

int main() {
    testFormatterIdempotencyUiStringsAndDirectives();
    testGovernanceLinterRulesAndSuppressions();
    testLanguageServiceNavigationUtf16AndSceneGuids();
    testLspServerFramingAndUtf16RoundTrip();
    testGovernanceFuzzerDifferentialAndMutation();
    testRscCliFormatAndLintSmoke();
    testProjectConfigMacroPluginsAndSkeletonFastpath();
    std::cout << "All app_tooling_and_lsp_tests passed!\n";
    return 0;
}
