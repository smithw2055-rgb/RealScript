#include "realscript/tooling/Formatter.h"
#include "realscript/tooling/GovernanceFuzzer.h"
#include "realscript/tooling/GovernanceLinter.h"
#include "realscript/tooling/Json.h"
#include "realscript/tooling/LanguageService.h"
#include "realscript/tooling/LspServer.h"

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

namespace {

#define RS_CHECK(expr)                                                         \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::ostringstream oss_;                                           \
            oss_ << "Check failed at " << __FILE__ << ":" << __LINE__          \
                 << ": " << #expr;                                             \
            throw std::runtime_error(oss_.str());                              \
        }                                                                      \
    } while (false)

#define RS_CHECK_EQ(actual, expected)                                          \
    do {                                                                       \
        const auto& act_ = (actual);                                           \
        const auto& exp_ = (expected);                                         \
        if (!(act_ == exp_)) {                                                 \
            std::ostringstream oss_;                                           \
            oss_ << "Equality check failed at " << __FILE__ << ":" << __LINE__ \
                 << "\n  expression: " << #actual << " == " << #expected       \
                 << "\n  actual:   [" << act_ << "]"                           \
                 << "\n  expected: [" << exp_ << "]";                          \
            throw std::runtime_error(oss_.str());                              \
        }                                                                      \
    } while (false)

bool containsCode(
    const std::vector<realscript::tooling::LintDiagnostic>& diags,
    std::string_view code) {
    for (const auto& d : diags) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

bool containsLangCode(
    const std::vector<realscript::tooling::LanguageDiagnostic>& diags,
    std::string_view code) {
    for (const auto& d : diags) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

std::int64_t compileAndRunInt(const std::string& source, const std::string& entry) {
    realscript::compiler::Compilation comp({{"script.rscript", source}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        for (const auto& d : build.diagnostics.items()) {
            std::cerr << "[compileAndRunInt] " << d.code << ": " << d.message << "\n";
        }
    }
    RS_CHECK(!build.diagnostics.hasErrors());
    std::vector<realscript::bytecode::Module> bcModules;
    realscript::bytecode::Lowerer lowerer;
    for (const auto& mirMod : build.modules) {
        auto bc = lowerer.lower(mirMod);
        RS_CHECK(realscript::bytecode::verifyModule(bc, build.diagnostics));
        bcModules.push_back(std::move(bc));
    }
    RS_CHECK(!build.diagnostics.hasErrors());
    realscript::runtime::Interpreter interp(std::move(bcModules));
    const auto exec = interp.invoke(entry, {});
    RS_CHECK(exec.succeeded);
    return std::stoll(realscript::runtime::valueToString(exec.value, interp.heap().get()));
}

void writeFileText(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    RS_CHECK(static_cast<bool>(out));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    RS_CHECK(static_cast<bool>(out));
}

std::string readFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    RS_CHECK(static_cast<bool>(in));
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct CommandOutput {
    int exitCode = -1;
    std::string combinedOutput;
};

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

CommandOutput runCommand(const std::vector<std::string>& args) {
    std::string cmd;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmd.push_back(' ');
        cmd += shellQuote(args[i]);
    }
    cmd += " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    RS_CHECK(pipe != nullptr);
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const int status = pclose(pipe);
    CommandOutput res;
    if (WIFEXITED(status)) {
        res.exitCode = WEXITSTATUS(status);
    } else {
        res.exitCode = status;
    }
    res.combinedOutput = std::move(output);
    return res;
}

void testFormatterCoreAndIdempotency() {
    using namespace realscript::tooling;

    const std::string messySource = R"(// File header comment
module   Game.Combat ;


/* Multi-line block comment
   preserving internal layout */
import   Game.Core ;




[Export]
public class DamageSystem {
[Export]
private int _basePower=10;

public string Describe(int raw,bool crit){
int   val = -raw + +_basePower;
val++;
--val;
bool ok=!crit && raw<100 && val>=0;
string msg = $"Raw={raw + 1}, Val={val}, Ok={ok}";
string tier = raw switch {
100 => "S",
50 => "A",
_ => "B"
};
var fn = (int x) => x * 2 + 1;
return msg + tier + /* inline comment */ $"/{fn(val)}"; // trailing comment
}

public int GenericAndCompare<T>(Dictionary<string,List<int>> table,int a,int b){
if(a<b&&b>0){
return a+b;
}
return 0;
}
}
)";

    const auto firstPass = formatSource(messySource);
    RS_CHECK(!firstPass.unchangedOnError);
    RS_CHECK(firstPass.changed);
    RS_CHECK(!firstPass.formatted.empty());
    RS_CHECK(firstPass.formatted.back() == '\n');

    // Comments and interpolated strings preserved
    RS_CHECK(firstPass.formatted.find("// File header comment") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("/* Multi-line block comment") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("/* inline comment */") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("// trailing comment") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("$\"Raw={raw + 1}, Val={val}, Ok={ok}\"") != std::string::npos);

    // Attributes, generics vs comparisons, unary/inc/dec, lambda, switch expression
    RS_CHECK(firstPass.formatted.find("[Export]\npublic class DamageSystem") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("Dictionary<string, List<int>>") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("GenericAndCompare<T>(") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("a < b && b > 0") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("raw < 100 && val >= 0") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("-raw + +_basePower") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("val++;") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("--val;") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("!crit") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("100 => \"S\"") != std::string::npos);
    RS_CHECK(firstPass.formatted.find("(int x) => x * 2 + 1") != std::string::npos);

    // Multiple blank lines collapsed to maxConsecutiveBlankLines = 1 (at most "\n\n")
    RS_CHECK(firstPass.formatted.find("\n\n\n") == std::string::npos);

    // Strict idempotency law
    const auto secondPass = formatSource(firstPass.formatted);
    RS_CHECK(!secondPass.unchangedOnError);
    RS_CHECK(!secondPass.changed);
    RS_CHECK_EQ(secondPass.formatted, firstPass.formatted);

    // Custom FormatOptions: indentWidth = 2, maxConsecutiveBlankLines = 0, insertFinalNewline = false
    FormatOptions customOpts;
    customOpts.indentWidth = 2;
    customOpts.maxConsecutiveBlankLines = 0;
    customOpts.insertFinalNewline = false;
    const auto customPass = formatSource("module Demo;\n\n\npublic class Box {\npublic int Get(){return 7;}\n}\n", customOpts);
    RS_CHECK(!customPass.unchangedOnError);
    RS_CHECK(!customPass.formatted.empty());
    RS_CHECK(customPass.formatted.back() != '\n');
    RS_CHECK(customPass.formatted.find("\n\n") == std::string::npos);
    RS_CHECK(customPass.formatted.find("\n  public int Get()") != std::string::npos);
    const auto customSecond = formatSource(customPass.formatted, customOpts);
    RS_CHECK_EQ(customSecond.formatted, customPass.formatted);

    // Severe lexical unclosed errors must return original input unchanged with unchangedOnError = true
    const std::vector<std::string> brokenInputs = {
        "module Broken;\npublic int Main() {\n    string s = \"unterminated string;\n    return 0;\n}",
        "module Broken;\npublic int Main() {\n    string s = $\"hello {1 + 2\";\n    return 0;\n}",
        "module Broken;\n/* unclosed block comment\npublic int Main() { return 1; }\n"
    };
    for (const auto& broken : brokenInputs) {
        const auto errRes = formatSource(broken);
        RS_CHECK(errRes.unchangedOnError);
        RS_CHECK(!errRes.changed);
        RS_CHECK(!errRes.errorMessage.empty());
        RS_CHECK_EQ(errRes.formatted, broken);
    }

    // Semantic preservation before & after formatting + idempotency across 20 fuzzer-generated programs
    const std::string runnableMessy =
        "module MathCheck; public int Main(){int acc=0;for(int i=0;i<10;i=i+1){acc+=i*2 - -1; acc+=1;} return acc;}";
    const auto formattedRunnable = formatSource(runnableMessy);
    RS_CHECK(!formattedRunnable.unchangedOnError);
    RS_CHECK_EQ(
        compileAndRunInt(runnableMessy, "MathCheck::Main"),
        compileAndRunInt(formattedRunnable.formatted, "MathCheck::Main"));

    for (std::size_t i = 0; i < 20; ++i) {
        const std::string generated = generateRandomProgram(0xCAFEBABE1234ULL, i);
        const auto f1 = formatSource(generated);
        RS_CHECK(!f1.unchangedOnError);
        const auto f2 = formatSource(f1.formatted);
        RS_CHECK(!f2.unchangedOnError);
        RS_CHECK(!f2.changed);
        RS_CHECK_EQ(f2.formatted, f1.formatted);
    }
}

void testGovernanceLinterRulesAndSuppression() {
    using namespace realscript::tooling;

    // RSG001: hot-path-allocation (new class, new array, heap closure in frame callbacks; struct allowed)
    {
        const std::string src = R"(module HotPathTest;

public struct Vec2 {
    public int x;
    public int y;
}

public class Enemy {
    public int hp = 10;
}

public class PlayerController {
    public void OnInit() {
        Enemy okInInit = new Enemy();
        int[] okArr = new int[4];
        if (okInInit.hp > 0 && okArr.Length > 0) {}
    }

    public void OnUpdate(float dt) {
        Vec2 stackValue = new Vec2();
        if (dt > 0.0f && stackValue.x == 0) {
            Enemy badObj = new Enemy();
            int[] badArr = new int[8];
            int factor = 3;
            var closure = (v) => v + factor;
            if (badObj.hp > 0 && badArr.Length > 0 && closure(1) > 0) {}
        }
    }
}
)";
        GovernanceLinter linter;
        const auto diags = linter.lintSource("player.rscript", src);
        int rsg001Count = 0;
        for (const auto& d : diags) {
            if (d.code == "RSG001") ++rsg001Count;
        }
        RS_CHECK_EQ(rsg001Count, 3);
    }

    // RSG002: cyclomatic-complexity (default 20 and custom threshold)
    {
        const std::string src = R"(module ComplexityTest;

public int ModerateBranching(int a, int b, int c) {
    int score = 0;
    if (a > 0 && b > 0) {
        score += 1;
    } else if (a < 0 || c > 0) {
        score += 2;
    }
    for (int i = 0; i < 3; i = i + 1) {
        if ((i + a) > 2) {
            score += i;
        }
    }
    return score > 0 ? score : 0;
}
)";
        GovernanceLinter defaultLinter;
        const auto defaultDiags = defaultLinter.lintSource("complex.rscript", src);
        RS_CHECK(!containsCode(defaultDiags, "RSG002"));

        LintConfig strictCfg;
        strictCfg.maxCyclomaticComplexity = 4;
        GovernanceLinter strictLinter(strictCfg);
        const auto strictDiags = strictLinter.lintSource("complex.rscript", src);
        RS_CHECK(containsCode(strictDiags, "RSG002"));
    }

    // RSG003: unused-symbol (unused local, unread private field, unreferenced import)
    {
        const std::vector<realscript::compiler::SourceFile> ws = {
            {"helper_a.rscript", "module Game.UsedMod;\npublic int Boost(int x) { return x + 1; }\n"},
            {"helper_b.rscript", "module Game.UnusedMod;\npublic int UnusedFn() { return 99; }\n"},
            {"actor.rscript", R"(module Game.Actor;
import Game.UsedMod;
import Game.UnusedMod;

public class Hero {
    private int _unreadSecret = 42;
    private int _activeArmor = 5;
    public int publicHealth = 100;

    public int Compute(int input) {
        int unusedLocal = 123;
        int _intentionallyIgnored = 456;
        return Boost(input) + _activeArmor;
    }
}
)"}
        };
        realscript::compiler::Compilation comp(ws);
        const auto build = comp.build();
        if (build.diagnostics.hasErrors()) { for (const auto& d : build.diagnostics.items()) std::cerr << "[ws build] " << d.code << ": " << d.message << "\n"; }
        RS_CHECK(!build.diagnostics.hasErrors());

        GovernanceLinter linter;
        const auto diags = linter.lintSource("actor.rscript", ws[2].content, &build, &ws);
        int rsg003Count = 0;
        bool sawUnusedImport = false;
        bool sawUnreadPrivate = false;
        bool sawUnusedLocal = false;
        for (const auto& d : diags) {
            if (d.code == "RSG003") {
                ++rsg003Count;
                if (d.message.find("Game.UnusedMod") != std::string::npos) sawUnusedImport = true;
                if (d.message.find("_unreadSecret") != std::string::npos) sawUnreadPrivate = true;
                if (d.message.find("unusedLocal") != std::string::npos) sawUnusedLocal = true;
            }
        }
        RS_CHECK(sawUnusedImport);
        RS_CHECK(sawUnreadPrivate);
        RS_CHECK(sawUnusedLocal);
        RS_CHECK_EQ(rsg003Count, 3);
    }

    // RSG004: sequence-float-equality (float/double == or != inside deterministic sequence)
    {
        const std::string src = R"(module SeqTest;

public class CutsceneRunner {
    public sequence PatrolSeq(float targetTime, int targetTick) {
        float elapsed = 0.0f;
        int tick = 0;
        if (elapsed == targetTime) {
            yield wait_ticks(1);
        }
        if (tick == targetTick) {
            yield wait_ticks(1);
        }
    }

    public bool NormalFloatCheck(float a, float b) {
        return a == b;
    }
}
)";
        GovernanceLinter linter;
        const auto diags = linter.lintSource("seq.rscript", src);
        int rsg004Count = 0;
        for (const auto& d : diags) {
            if (d.code == "RSG004") ++rsg004Count;
        }
        RS_CHECK_EQ(rsg004Count, 1);
    }

    // RSG005: suspicious-control-flow (uncommented empty catch & identical if/else branches)
    {
        const std::string src = R"(module FlowTest;

public int CheckFlow(bool flag, int x) {
    int result = 0;
    try {
        result = x + 1;
    } catch (string err) {
    }

    try {
        result = x + 2;
    } catch (string ignoredErr) {
        // Intentionally ignored fallback path
    }

    if (flag) {
        result = result + 10;
    } else {
        result = result + 10;
    }
    return result;
}
)";
        GovernanceLinter linter;
        const auto diags = linter.lintSource("flow.rscript", src);
        int rsg005Count = 0;
        for (const auto& d : diags) {
            if (d.code == "RSG005") ++rsg005Count;
        }
        RS_CHECK_EQ(rsg005Count, 2);
    }

    // Project JSON config + inline comment suppression (single, comma-separated, blanket)
    {
        const std::string configJson = R"({
            "enableGovernanceLint": true,
            "lint": {
                "maxCyclomaticComplexity": 15,
                "rules": {
                    "RSG001": "error",
                    "RSG003": "off",
                    "suspicious-control-flow": "info"
                }
            }
        })";
        std::string parseErr;
        const auto parsed = Json::parse(configJson, parseErr);
        RS_CHECK(parsed.has_value());
        LintConfig cfg;
        RS_CHECK(parseLintConfigFromProjectJson(*parsed, cfg, false));
        RS_CHECK(cfg.enableGovernanceLint);
        RS_CHECK_EQ(cfg.maxCyclomaticComplexity, static_cast<std::size_t>(15));
        RS_CHECK(cfg.severityFor("RSG001") == LintSeverity::Error);
        RS_CHECK(cfg.severityFor("RSG003") == LintSeverity::Off);
        RS_CHECK(cfg.severityFor("RSG005") == LintSeverity::Info);

        const std::string suppressedSrc = R"(module SuppressTest;

public class Dummy {
    public int v = 1;
}

public class Updater {
    public void Update() {
        int unusedButRuleOff = 7;
        // realscript-disable-next-line RSG001
        Dummy suppressedAlloc = new Dummy();
        Dummy unsuppressedAlloc = new Dummy();
        // realscript-disable-next-line RSG001, RSG005
        if (suppressedAlloc.v > 0) { unsuppressedAlloc.v = 2; } else { unsuppressedAlloc.v = 2; }
    }
}
)";
        GovernanceLinter linter(cfg);
        const auto diags = linter.lintSource("suppress.rscript", suppressedSrc);
        RS_CHECK_EQ(diags.size(), static_cast<std::size_t>(1));
        RS_CHECK_EQ(diags[0].code, std::string("RSG001"));
        RS_CHECK(diags[0].severity == LintSeverity::Error);
    }
}

void testLspFormattingLintAndQuickFix() {
    using namespace realscript::tooling;

    LspServer server;
    std::ostringstream notifications;

    auto initResp = server.handle(Json::Object{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", Json::Object{}}
    }, &notifications);

    RS_CHECK(initResp["result"]["capabilities"]["documentFormattingProvider"].boolValue(false));
    RS_CHECK(initResp["result"]["capabilities"]["codeActionProvider"].isObject() || initResp["result"]["capabilities"]["codeActionProvider"].boolValue(false));

    const std::string uri = "file:///workspace/unit_script.rscript";
    const std::string docPath = "/workspace/unit_script.rscript";
    const std::string unformattedWithHotAlloc =
        "module LspDemo;\n"
        "public class Box { public int x = 1; }\n"
        "public class Actor {\n"
        "public void OnUpdate(){\n"
        "Box b = new Box();\n"
        "if (b.x > 0) {}\n"
        "}\n"
        "}\n";

    // Default: LanguageService::diagnostics() must NOT inject RSG* unless enableGovernanceLint is true
    (void)server.handle(Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", Json::Object{
            {"textDocument", Json::Object{
                {"uri", uri},
                {"languageId", "realscript"},
                {"version", 1},
                {"text", unformattedWithHotAlloc}
            }}
        }}
    }, &notifications);

    RS_CHECK(!containsLangCode(server.service().diagnostics(docPath), "RSG001"));
    RS_CHECK(containsLangCode(server.service().lint(docPath), "RSG001"));

    // Enable governance lint via realscript.project.json config
    RS_CHECK(server.service().loadProjectConfigJson(
        R"({"enableGovernanceLint": true, "lint": {"rules": {"RSG001": "warning"}}})"));
    const auto mergedDiags = server.service().diagnostics(docPath);
    RS_CHECK(containsLangCode(mergedDiags, "RSG001"));

    // Request textDocument/formatting via LSP
    auto fmtResp = server.handle(Json::Object{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "textDocument/formatting"},
        {"params", Json::Object{
            {"textDocument", Json::Object{{"uri", uri}}},
            {"options", Json::Object{{"tabSize", 4}, {"insertSpaces", true}}}
        }}
    }, &notifications);

    auto& fmtEdits = fmtResp["result"].arrayValue();
    RS_CHECK(!fmtEdits.empty());
    const std::string formattedText = fmtEdits[0]["newText"].stringValue();
    RS_CHECK(formattedText.find("    public void OnUpdate() {\n        Box b = new Box();") != std::string::npos);

    // Update document with formatted text
    server.service().update(docPath, formattedText, 2);

    // Locate RSG001 diagnostic line and request textDocument/codeAction Quick Fix
    const auto postFmtDiags = server.service().diagnostics(docPath);
    std::size_t rsgLine = 0;
    bool foundRsg001 = false;
    for (const auto& d : postFmtDiags) {
        if (d.code == "RSG001") {
            rsgLine = d.location.range.start.line;
            foundRsg001 = true;
            break;
        }
    }
    RS_CHECK(foundRsg001);

    auto caResp = server.handle(Json::Object{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "textDocument/codeAction"},
        {"params", Json::Object{
            {"textDocument", Json::Object{{"uri", uri}}},
            {"range", Json::Object{
                {"start", Json::Object{{"line", static_cast<std::int64_t>(rsgLine)}, {"character", 8}}},
                {"end", Json::Object{{"line", static_cast<std::int64_t>(rsgLine)}, {"character", 16}}}
            }}
        }}
    }, &notifications);

    auto& actions = caResp["result"].arrayValue();
    RS_CHECK(!actions.empty());
    std::string insertedSuppression;
    std::size_t insertLine = 0;
    for (auto& act : actions) {
        if (act["title"].stringValue().find("realscript-disable-next-line RSG001") != std::string::npos) {
            auto& docEdits = act["edit"]["changes"][uri].arrayValue();
            RS_CHECK_EQ(docEdits.size(), static_cast<std::size_t>(1));
            insertedSuppression = docEdits[0]["newText"].stringValue();
            insertLine = static_cast<std::size_t>(docEdits[0]["range"]["start"]["line"].integerValue());
            break;
        }
    }
    RS_CHECK(!insertedSuppression.empty());
    RS_CHECK_EQ(insertLine, rsgLine);
    RS_CHECK(insertedSuppression.find("        // realscript-disable-next-line RSG001\n") != std::string::npos);

    // Apply Quick Fix via LSP textDocument/didChange and verify RSG001 is suppressed
    (void)server.handle(Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didChange"},
        {"params", Json::Object{
            {"textDocument", Json::Object{{"uri", uri}, {"version", 3}}},
            {"contentChanges", Json::Array{
                Json::Object{
                    {"range", Json::Object{
                        {"start", Json::Object{{"line", static_cast<std::int64_t>(insertLine)}, {"character", 0}}},
                        {"end", Json::Object{{"line", static_cast<std::int64_t>(insertLine)}, {"character", 0}}}
                    }},
                    {"text", insertedSuppression}
                }
            }}
        }}
    }, &notifications);
    RS_CHECK(!containsLangCode(server.service().diagnostics(docPath), "RSG001"));
}

void testRscCliIntegration(const char* argv0) {
    const std::filesystem::path selfExe = std::filesystem::absolute(std::filesystem::path(argv0));
    const std::filesystem::path rscExe = selfExe.parent_path() / "rsc";
    RS_CHECK(std::filesystem::exists(rscExe));

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path scratchDir =
        std::filesystem::temp_directory_path() / ("realscript_p3_s2_cli_" + std::to_string(stamp));
    std::filesystem::create_directories(scratchDir);

    struct CleanupGuard {
        std::filesystem::path dir;
        ~CleanupGuard() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    } cleanup{scratchDir};

    const std::filesystem::path sampleFile = scratchDir / "cli_sample.rscript";
    const std::string unformattedCode =
        "module CliDemo;\n"
        "public class Item { public int id = 7; }\n"
        "public class Runner {\n"
        "public void OnUpdate(){\n"
        "Item it=new Item();\n"
        "if(it.id>0){}\n"
        "}\n"
        "}\n"
        "public int Main(){return 42;}\n";
    writeFileText(sampleFile, unformattedCode);

    // 1. --check-format fails on unformatted file
    {
        const auto res = runCommand({rscExe.string(), sampleFile.string(), "--check-format"});
        RS_CHECK_EQ(res.exitCode, 1);
    }

    // 2. --format (stdout) prints formatted text without modifying original file on disk
    {
        const auto res = runCommand({rscExe.string(), sampleFile.string(), "--format"});
        RS_CHECK_EQ(res.exitCode, 0);
        RS_CHECK(res.combinedOutput.find("    public void OnUpdate() {") != std::string::npos);
        RS_CHECK_EQ(readFileText(sampleFile), unformattedCode);
    }

    // 3. --format --write formats in-place, after which --check-format returns 0
    {
        const auto writeRes = runCommand({rscExe.string(), sampleFile.string(), "--format", "--write"});
        RS_CHECK_EQ(writeRes.exitCode, 0);
        const auto checkRes = runCommand({rscExe.string(), sampleFile.string(), "--check-format"});
        RS_CHECK_EQ(checkRes.exitCode, 0);
    }

    // 4. --lint emits RSG001 warning with exit code 0; --lint-deny-warnings returns exit code 1
    {
        const auto lintRes = runCommand({rscExe.string(), sampleFile.string(), "--lint"});
        RS_CHECK_EQ(lintRes.exitCode, 0);
        RS_CHECK(lintRes.combinedOutput.find("RSG001") != std::string::npos);

        const auto denyRes = runCommand({rscExe.string(), sampleFile.string(), "--lint", "--lint-deny-warnings"});
        RS_CHECK_EQ(denyRes.exitCode, 1);
    }

    // 5. realscript.project.json auto-discovery elevating RSG001 to "error" -> --lint returns exit code 1
    {
        const std::filesystem::path cfgPath = scratchDir / "realscript.project.json";
        writeFileText(cfgPath, R"({"lint": {"rules": {"RSG001": "error"}}})");
        const auto errLintRes = runCommand({rscExe.string(), sampleFile.string(), "--lint"});
        RS_CHECK_EQ(errLintRes.exitCode, 1);
        RS_CHECK(errLintRes.combinedOutput.find("error RSG001") != std::string::npos);
    }

    // 6. Existing compilation/execution options remain 100% compatible
    {
        const auto runRes = runCommand({rscExe.string(), sampleFile.string(), "--run", "CliDemo::Main"});
        RS_CHECK_EQ(runRes.exitCode, 0);
        RS_CHECK(runRes.combinedOutput.find("42") != std::string::npos);
    }
}

void testGovernanceDifferentialAndBytecodeMutationFuzzer() {
    using namespace realscript::tooling;

    // Deterministic generator reproducibility
    const std::string progA1 = generateRandomProgram(0x13579BDF2468ACE0ULL, 3);
    const std::string progA2 = generateRandomProgram(0x13579BDF2468ACE0ULL, 3);
    const std::string progB = generateRandomProgram(0x13579BDF2468ACE0ULL, 4);
    RS_CHECK_EQ(progA1, progA2);
    RS_CHECK(progA1 != progB);

    // Multi-backend differential fuzzing: Interpreter vs StencilJit (Native x86_64 & Threaded Stencils)
    DifferentialFuzzOptions diffOpts;
    diffOpts.seed = 0x525350335332ULL;
    diffOpts.programCount = 24;
    diffOpts.inputsPerFunction = 8;
    diffOpts.instructionBudget = 200000;
    const auto diffReport = runDifferentialFuzz(diffOpts);
    if (!diffReport.failures.empty()) {
        for (const auto& f : diffReport.failures) {
            std::cerr << "[DiffFuzz failure] " << f << '\n';
        }
    }
    RS_CHECK(diffReport.succeeded());
    RS_CHECK_EQ(diffReport.programsGenerated, static_cast<std::size_t>(24));
    RS_CHECK(diffReport.functionsTested >= 72);
    RS_CHECK(diffReport.invocationsExecuted >= 500);
    RS_CHECK(diffReport.threadedFunctionsCompiled > 0);
#if defined(__x86_64__) || defined(_M_X64)
    RS_CHECK(diffReport.nativeX64FunctionsCompiled > 0);
#endif

    // Bytecode (.rsbc) binary mutation fuzzing: bit-flips, truncation, magic/version tamper, OOB reg/block tamper
    std::cout << "  [DiffFuzz stats] programs=" << diffReport.programsGenerated
              << " functions=" << diffReport.functionsTested
              << " invocations=" << diffReport.invocationsExecuted
              << " nativeX64=" << diffReport.nativeX64FunctionsCompiled
              << " threaded=" << diffReport.threadedFunctionsCompiled
              << " runtimeErrorsMatched=" << diffReport.runtimeErrorsMatched << '\n';

    BytecodeMutationFuzzOptions bcOpts;
    bcOpts.seed = 0x52534243465AULL;
    bcOpts.mutationsPerSeedModule = 250;
    const auto bcReport = runBytecodeMutationFuzz(bcOpts);
    if (!bcReport.failures.empty()) {
        for (const auto& f : bcReport.failures) {
            std::cerr << "[BytecodeFuzz failure] " << f << '\n';
        }
    }
    std::cout << "  [BytecodeFuzz stats] total=" << bcReport.totalMutations
              << " bitFlip=" << bcReport.bitFlipMutations
              << " trunc=" << bcReport.truncationMutations
              << " header=" << bcReport.headerVersionMutations
              << " oobReg=" << bcReport.outOfBoundsRegisterMutations
              << " oobBlock=" << bcReport.outOfBoundsBlockMutations
              << " rejectedSafely=" << bcReport.rejectedSafelyCount << '\n';
    RS_CHECK(bcReport.succeeded());
    RS_CHECK(bcReport.totalMutations >= 500);
    RS_CHECK(bcReport.bitFlipMutations > 0);
    RS_CHECK(bcReport.truncationMutations > 0);
    RS_CHECK(bcReport.headerVersionMutations > 0);
    RS_CHECK(bcReport.outOfBoundsRegisterMutations > 0);
    RS_CHECK(bcReport.outOfBoundsBlockMutations > 0);
    RS_CHECK(bcReport.rejectedSafelyCount > 0);
}

} // namespace

int main(int argc, char** argv) {
    (void)argc;
    try {
        std::cout << "[1/5] Testing deterministic Formatter & idempotency...\n";
        testFormatterCoreAndIdempotency();

        std::cout << "[2/5] Testing configurable GovernanceLinter rules RSG001-RSG005 & suppressions...\n";
        testGovernanceLinterRulesAndSuppression();

        std::cout << "[3/5] Testing LanguageService & LspServer formatting, config & Quick Fix...\n";
        testLspFormattingLintAndQuickFix();

        std::cout << "[4/5] Testing rsc CLI (--format, --check-format, --lint, --lint-deny-warnings)...\n";
        testRscCliIntegration(argv[0]);

        std::cout << "[5/5] Testing multi-backend Differential Fuzzer & .rsbc Bytecode Mutation Fuzzer...\n";
        testGovernanceDifferentialAndBytecodeMutationFuzzer();

        std::cout << "All P3 Stage 2 formatter, governance linter, LSP/CLI, and fuzzer tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "TEST FAILURE: " << ex.what() << '\n';
        return 1;
    }
}
