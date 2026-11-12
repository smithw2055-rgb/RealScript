#include "realscript/debug/DapServer.h"
#include "realscript/debug/Debugger.h"
#include "realscript/game/GameProductization.h"
#include "realscript/game/GameScripting.h"
#include "realscript/tooling/Json.h"
#include "realscript/tooling/LanguageService.h"
#include "realscript/tooling/LspServer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace realscript;

namespace {

void require(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error("P1 test failure: " + msg);
    }
}

// ============================================================================
// 1. P1-A: DAP Watch / Hover / Conditional Breakpoint Evaluator & Pure Sandbox
// ============================================================================
void testDapEvaluatorAndPureGetterSandbox() {
    std::cout << "[P1-A] Testing DAP Watch/Hover/Conditional Breakpoint Evaluator & Pure Getter Sandbox...\n";

    game::GameApi api;
    const char* src = R"RS(
module P1.DapEval;
import RealScript.Math;

public enum BattleRank {
    Rookie = 1,
    Veteran = 5,
    Legend = 10
}

public class GameConfig {
    public const int BaseCap = 100;
    public static int GlobalBonus = 25;
    public static int ImpureCounter = 0;

    public static int PureScale(int x) {
        return x * 2 + GlobalBonus;
    }

    public static int MutateStatic() {
        ImpureCounter = ImpureCounter + 1;
        return ImpureCounter;
    }
}

public struct ArmorStats {
    public int defense;
    public int resist;
    public ArmorStats(int d, int r) { defense = d; resist = r; }
}

public class Actor {
    public string title = "Paladin";
    public int baseAtk = 40;
    public ArmorStats armor;
    public Vec3 position;
    public int? buff;
    public int? emptyBuff;
    public List<int> items;
    public Queue<int> cooldowns;
    public Stack<int> states;
    public Dictionary<string, int> currencies;
    public HashSet<int> perks;

    public int Level { get; set; }

    public int EffectiveAttack {
        get {
            return baseAtk + Level * 5 + GameConfig.GlobalBonus;
        }
    }

    public int ImpureDamageProperty {
        get {
            baseAtk = baseAtk + 999;
            return baseAtk;
        }
    }

    public Actor() {
        armor = new ArmorStats(30, 15);
        position = Vec3.Of(6, 8, 10);
        buff = 12;
        emptyBuff = null;
        Level = 4;

        items = new List<int>(8);
        items.Add(11);
        items.Add(22);
        items.Add(33);

        // Ring-buffer wrap-around: cap=4, enqueue 3, dequeue 2 -> head=2, enqueue 2 -> logical [300, 400, 500]
        cooldowns = new Queue<int>(4);
        cooldowns.Enqueue(100);
        cooldowns.Enqueue(200);
        cooldowns.Enqueue(300);
        int a = cooldowns.Dequeue();
        int b = cooldowns.Dequeue();
        cooldowns.Enqueue(400 + a * 0 + b * 0);
        cooldowns.Enqueue(500);

        states = new Stack<int>(4);
        states.Push(7);
        states.Push(8);
        states.Push(9);

        currencies = new Dictionary<string, int>(8);
        currencies["gold"] = 550;
        currencies["gems"] = 35;

        perks = new HashSet<int>(8);
        perks.Add(42);
    }

    public int ComputeCrit(int mult) {
        return EffectiveAttack * mult + armor.defense;
    }

    public int MutateBaseAtk(int delta) {
        baseAtk = baseAtk + delta;
        return baseAtk;
    }

    public int RunInfiniteRecursion(int x) {
        return RunInfiniteRecursion(x + 1);
    }
}

public int Entry() {
    GameConfig.GlobalBonus = 25;
    Actor hero = new Actor();
    Actor ghost = null;
    int sum = 0;
    for (int step = 0; step < 5; step = step + 1) {
        sum = sum + hero.items[0] + step;
    }
    return sum + (ghost == null ? 0 : 1);
}
)RS";

    game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"p1_dap.rscript", src}});
    require(compiled.succeeded(), "P1 DAP script failed to compile");

    // Locate the line of `sum = sum + hero.items[0] + step;` inside the loop
    std::size_t loopBodyLine = 0;
    {
        std::istringstream ss(src);
        std::string line;
        std::size_t ln = 0;
        while (std::getline(ss, line)) {
            ++ln;
            if (line.find("sum = sum + hero.items[0] + step;") != std::string::npos) {
                loopBodyLine = ln; // 1-based for DAP setBreakpoints
                break;
            }
        }
    }
    require(loopBodyLine > 0, "failed to locate loop line in p1_dap.rscript");

    debug::DapServer dap(compiled.program.program(), compiled.program.heap());
    std::ostringstream events;

    // Set conditional breakpoint using upgraded container + computed property + null-coalescing expression!
    const auto bpResp = dap.handle(tooling::Json::Object{
        {"seq", 1},
        {"type", "request"},
        {"command", "setBreakpoints"},
        {"arguments", tooling::Json::Object{
            {"source", tooling::Json::Object{{"path", "p1_dap.rscript"}}},
            {"breakpoints", tooling::Json::Array{
                tooling::Json::Object{
                    {"line", static_cast<std::int64_t>(loopBodyLine)},
                    {"column", 1},
                    {"condition", "ghost == null && hero.EffectiveAttack == 85 && hero.currencies[\"gold\"] == 550 && step == 3"},
                },
            }},
        }},
    }, &events);
    require(bpResp.find("success")->boolValue(), "setBreakpoints failed");

    const auto launchResp = dap.handle(tooling::Json::Object{
        {"seq", 2},
        {"type", "request"},
        {"command", "launch"},
        {"arguments", tooling::Json::Object{
            {"function", "P1.DapEval::Entry"},
        }},
    }, &events);
    require(launchResp.find("success")->boolValue(), "DAP launch failed");
    require(events.str().find("\"event\":\"stopped\"") != std::string::npos,
        "Conditional breakpoint did not trigger stop");

    int seq = 10;
    const auto evalExpr = [&](const std::string& expr, bool expectSuccess = true) -> tooling::Json {
        auto resp = dap.handle(tooling::Json::Object{
            {"seq", seq++},
            {"type", "request"},
            {"command", "evaluate"},
            {"arguments", tooling::Json::Object{
                {"expression", expr},
                {"frameId", 1},
            }},
        }, &events);
        if (expectSuccess && !resp.find("success")->boolValue()) {
            const auto* msg = resp.find("message");
            throw std::runtime_error(
                "Expected evaluate('" + expr + "') to succeed, got error: " +
                (msg ? msg->stringValue() : "<none>"));
        }
        return resp;
    };
    const auto evalResultStr = [&](const std::string& expr) -> std::string {
        return evalExpr(expr, true).find("body")->find("result")->stringValue();
    };

    // Verify conditional breakpoint stopped on exact iteration step == 3!
    require(evalResultStr("step") == "3", "Conditional breakpoint should stop at step == 3, got " + evalResultStr("step"));

    // 1. Null-conditional `?.`, `?[`, null-coalescing `??`, ternary `?:`, short-circuit `&&` / `||`
    require(evalResultStr("ghost?.armor.defense ?? 777") == "777", "ghost?.armor.defense ?? 777 failed");
    require(evalResultStr("ghost?.items?[0] ?? 99") == "99", "ghost?.items?[0] ?? 99 failed");
    require(evalResultStr("hero?.items?[1] ?? 0") == "22", "hero?.items?[1] ?? 0 failed");
    require(evalResultStr("ghost != null && ghost.armor.defense > 0") == "false", "short-circuit && failed");
    require(evalResultStr("hero != null || ghost.armor.defense > 0") == "true", "short-circuit || failed");
    require(evalResultStr("hero.buff ?? -1") == "12", "non-empty Nullable ?? failed");
    require(evalResultStr("hero.emptyBuff ?? 404") == "404", "empty Nullable ?? failed");
    require(evalResultStr("hero.buff.HasValue && !hero.emptyBuff.HasValue") == "true", "Nullable.HasValue failed");
    require(evalResultStr("hero.emptyBuff.GetValueOrDefault(88)") == "88", "Nullable.GetValueOrDefault failed");

    // 2. Monomorphized container indexing & logical queries
    require(evalResultStr("hero.items.Count") == "3", "List.Count failed");
    require(evalResultStr("hero.items.Capacity >= 8") == "true", "List.Capacity failed");
    require(evalResultStr("!hero.items.IsEmpty && hero.items.Contains(22)") == "true", "List.Contains failed");
    require(evalResultStr("hero.cooldowns.Count") == "3", "Queue.Count failed");
    require(evalResultStr("hero.cooldowns[0]") == "300", "Queue wrap-around [0] failed");
    require(evalResultStr("hero.cooldowns[2]") == "500", "Queue wrap-around [2] failed");
    require(evalResultStr("hero.cooldowns.Peek()") == "300", "Queue.Peek() failed");
    require(evalResultStr("hero.cooldowns.Contains(400) && !hero.cooldowns.Contains(100)") == "true", "Queue.Contains failed");
    require(evalResultStr("hero.states[0]") == "9", "Stack top-first [0] failed");
    require(evalResultStr("hero.states[2]") == "7", "Stack bottom [2] failed");
    require(evalResultStr("hero.states.Peek()") == "9", "Stack.Peek() failed");
    require(evalResultStr("hero.currencies[\"gold\"] + hero.currencies[\"gems\"]") == "585", "Dictionary[\"key\"] failed");
    require(evalResultStr("hero.currencies.ContainsKey(\"gold\") && hero.currencies.ContainsValue(35)") == "true",
        "Dictionary.ContainsKey/ContainsValue failed");
    require(evalResultStr("hero.perks.Contains(42) && !hero.perks.Contains(13)") == "true", "HashSet.Contains failed");

    // 3. String & Inline Math Vec3 / Fixed introspection
    require(evalResultStr("hero.title[0]") == "P", "string[0] failed");
    require(evalResultStr("hero.title.StartsWith(\"Pal\") && hero.title.EndsWith(\"din\")") == "true",
        "string StartsWith/EndsWith failed");
    require(evalResultStr("hero.position.x.ToInt() + hero.position[2].ToInt()") == "16",
        "Vec3 .x / [2] / Fixed.ToInt() failed");
    require(evalResultStr("hero.position.x.raw") == std::to_string(6 * 256), "Fixed.raw failed");

    // 4. Static fields, const fields, enum members, auto-properties, pure getters & pure methods
    require(evalResultStr("GameConfig.BaseCap + GameConfig.GlobalBonus") == "125", "Static/const field eval failed");
    require(evalResultStr("BattleRank.Legend == 10") == "true", "Enum member eval failed");
    require(evalResultStr("GameConfig.PureScale(10)") == "45", "Pure static method eval failed");
    require(evalResultStr("hero.Level") == "4", "Auto-property backing slot read failed");
    require(evalResultStr("hero.EffectiveAttack") == "85", "Computed pure property getter failed");
    require(evalResultStr("hero.ComputeCrit(2)") == "200", "Pure instance method calling pure getter failed");

    // 5. Impure / mutating getters & methods MUST be rejected without mutating debuggee state!
    const auto impureProp = evalExpr("hero.ImpureDamageProperty", false);
    require(!impureProp.find("success")->boolValue() &&
            impureProp.find("message")->stringValue().find("side effects") != std::string::npos,
        "Mutating property getter must be blocked by purity verifier");
    const auto impureMeth = evalExpr("hero.MutateBaseAtk(500)", false);
    require(!impureMeth.find("success")->boolValue(), "Mutating method must be blocked");
    const auto impureStatic = evalExpr("GameConfig.MutateStatic()", false);
    require(!impureStatic.find("success")->boolValue(), "Mutating static method must be blocked");

    // Verify debuggee state remained 100% untouched!
    require(evalResultStr("hero.baseAtk") == "40", "Debuggee hero.baseAtk was mutated!");
    require(evalResultStr("GameConfig.ImpureCounter") == "0", "Debuggee GameConfig.ImpureCounter was mutated!");

    // Bounded budget protection against infinite recursion in pure method
    const auto infRec = evalExpr("hero.RunInfiniteRecursion(1)", false);
    require(!infRec.find("success")->boolValue(),
        "Infinite recursion in watch expression must terminate cleanly via sandbox limits");

    (void)dap.handle(tooling::Json::Object{
        {"seq", seq++}, {"type", "request"}, {"command", "disconnect"},
    }, &events);
}

// ============================================================================
// 2. P1-B: LSP codeAction, prepareRename, and Scene/Signal/AssetGuid Bridge
// ============================================================================
void testLspCodeActionsPrepareRenameAndSceneManifest() {
    std::cout << "[P1-B] Testing LSP codeAction, prepareRename, and Scene/Signal/AssetGuid Manifest Bridge...\n";

    const auto tmpRoot = std::filesystem::temp_directory_path() / "realscript_p1b_ws_test";
    std::error_code ec;
    std::filesystem::remove_all(tmpRoot, ec);
    std::filesystem::create_directories(tmpRoot / "scenes", ec);
    std::filesystem::create_directories(tmpRoot / "prefabs", ec);
    std::filesystem::create_directories(tmpRoot / "scripts", ec);

    // 1. Write realscript.project.json with ambient stub + manifest resources/signals
    {
        std::ofstream out(tmpRoot / "realscript.project.json");
        out << R"JSON({
  "ambientStubs": [
    {
      "path": "engine_api.d.rscript",
      "content": "module Engine.Scene;\npublic int FindEntity(string nameOrPath);\npublic int GetNode(string path);\npublic void EmitSignal(string signalName);\npublic void ConnectSignal(string signalName, string method);\npublic class ResourceServer { public static int Load(string path); }\n"
    }
  ],
  "signals": [
    { "name": "on_boss_enraged", "ownerType": "Game.Boss::BossAi", "signature": "(int phase)" }
  ],
  "resources": [
    { "path": "res://audio/boss_theme.ogg", "guid": "11111111-2222-3333-4444-555555555555", "resourceType": "AudioStream" }
  ]
})JSON";
    }

    // 2. Write a Sprint 2 native `realscript.scene.text.v1` file AND a `.prefab.txt` file
    {
        std::ofstream out(tmpRoot / "scenes" / "level1.scene.txt");
        out << "# RealScript Git-Friendly Scene v1\n"
            << "format = realscript.scene.text.v1\n"
            << "schema_version = 1\n"
            << "entity.10.name = \"WorldRoot\"\n"
            << "entity.10.active = true\n"
            << "entity.25.name = \"PlayerHero\"\n"
            << "entity.25.parent = 10\n"
            << "entity.25.component.aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee.type = \"Game.Combat::HeroController\"\n"
            << "signal.0.name = \"on_shield_broken\"\n"
            << "signal.0.src_comp = \"Game.Combat::HeroController\"\n";
    }
    {
        std::ofstream out(tmpRoot / "prefabs" / "Turret.prefab.txt");
        out << "# RealScript Git-Friendly Prefab v1\n"
            << "format = realscript.prefab.text.v1\n"
            << "schema_version = 1\n"
            << "node.\"$root\".name = \"DefenseTurret\"\n"
            << "node.\"$root\".component.0.type = \"Game.Combat::TurretComp\"\n";
    }

    // 3. Write an exported utility module on disk
    {
        std::ofstream out(tmpRoot / "scripts" / "math_ext.rscript");
        out << "module Game.MathExt;\n"
            << "public int ClampPositive(int v) { return v < 0 ? 0 : v; }\n";
    }

    tooling::LspServer lsp;
    std::ostringstream notifications;
    const std::string rootUri = tooling::pathToUri(tmpRoot.generic_string());

    const auto initResp = lsp.handle(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "initialize"},
        {"params", tooling::Json::Object{
            {"rootUri", rootUri},
        }},
    }, &notifications);

    const auto* caps = initResp.find("result")->find("capabilities");
    require(caps && caps->find("codeActionProvider") != nullptr, "codeActionProvider capability missing");
    require(caps->find("renameProvider") && caps->find("renameProvider")->isObject() &&
            caps->find("renameProvider")->find("prepareProvider")->boolValue(),
        "renameProvider.prepareProvider capability missing");
    require(caps->find("executeCommandProvider") != nullptr, "executeCommandProvider capability missing");

    // Verify .scene.txt / .prefab.txt did NOT pollute script documentPaths()!
    for (const auto& dp : lsp.service().documentPaths()) {
        require(dp.find(".scene.txt") == std::string::npos && dp.find(".prefab.txt") == std::string::npos,
            "Scene/prefab manifest files must not enter script compilation documentPaths(): " + dp);
    }

    // 4. Open a script with:
    //    (a) Unimported `ClampPositive` call -> test Auto-Import Quick Fix!
    //    (b) Unimplemented interface `IDamageable` -> test Implement Interface Quick Fix!
    const std::string combatPath = (tmpRoot / "scripts" / "combat.rscript").generic_string();
    const std::string combatUri = tooling::pathToUri(combatPath);
    const std::string initialCombatSrc =
        "module Game.Combat;\n"
        "import Engine.Scene;\n"
        "\n"
        "[Guid(\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\")]\n"
        "public interface IDamageable {\n"
        "    int ApplyHit(int damage, bool critical);\n"
        "}\n"
        "\n"
        "public class HeroController : IDamageable {\n"
        "    public int hp = 100;\n"
        "    public int Heal(int x) {\n"
        "        return ClampPositive(x);\n"
        "    }\n"
        "}\n";

    (void)lsp.handle(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{
                {"uri", combatUri},
                {"version", 1},
                {"text", initialCombatSrc},
            }},
        }},
    }, &notifications);

    require(!lsp.service().diagnostics(combatPath).empty(),
        "combat.rscript should initially report missing import & missing interface implementation");

    // Request codeAction on line 11 (`return ClampPositive(x);`) -> Auto-import `Game.MathExt`
    const auto importActionsResp = lsp.handle(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "textDocument/codeAction"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{{"uri", combatUri}}},
            {"range", tooling::Json::Object{
                {"start", tooling::Json::Object{{"line", 11}, {"character", 16}}},
                {"end", tooling::Json::Object{{"line", 11}, {"character", 25}}},
            }},
        }},
    });
    const auto& importActions = importActionsResp.find("result")->arrayValue();
    bool foundAutoImport = false;
    std::string autoImportInsert;
    for (const auto& act : importActions) {
        if (act.find("title")->stringValue().find("Import 'Game.MathExt'") != std::string::npos) {
            foundAutoImport = true;
            const auto& edits = act.find("edit")->find("changes")->find(combatUri)->arrayValue();
            require(!edits.empty(), "Auto-import edit list is empty");
            autoImportInsert = edits.front().find("newText")->stringValue();
        }
    }
    require(foundAutoImport && autoImportInsert == "import Game.MathExt;\n",
        "Auto-import codeAction did not produce 'import Game.MathExt;'");

    // Request codeAction on `HeroController : IDamageable` (line 8) -> Implement interface `IDamageable`
    const auto ifaceActionsResp = lsp.handle(tooling::Json::Object{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "textDocument/codeAction"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{{"uri", combatUri}}},
            {"range", tooling::Json::Object{
                {"start", tooling::Json::Object{{"line", 8}, {"character", 10}}},
                {"end", tooling::Json::Object{{"line", 8}, {"character", 30}}},
            }},
        }},
    });
    const auto& ifaceActions = ifaceActionsResp.find("result")->arrayValue();
    bool foundImplementIface = false;
    std::string ifaceStubText;
    for (const auto& act : ifaceActions) {
        if (act.find("title")->stringValue().find("Implement interface 'IDamageable'") != std::string::npos) {
            foundImplementIface = true;
            const auto& edits = act.find("edit")->find("changes")->find(combatUri)->arrayValue();
            require(!edits.empty(), "Implement-interface edit list is empty");
            ifaceStubText = edits.front().find("newText")->stringValue();
        }
    }
    require(foundImplementIface &&
            ifaceStubText.find("public int ApplyHit(int damage, bool critical)") != std::string::npos &&
            ifaceStubText.find("return 0;") != std::string::npos,
        "Implement-interface codeAction did not emit valid C#-style RealScript method stub:\n" + ifaceStubText);

    // Apply both Quick Fixes + scene/signal/resource queries and verify ZERO diagnostics!
    const std::string fixedCombatSrc =
        "module Game.Combat;\n"
        "import Engine.Scene;\n"
        "import Game.MathExt;\n"
        "\n"
        "[Guid(\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\")]\n"
        "public interface IDamageable {\n"
        "    int ApplyHit(int damage, bool critical);\n"
        "}\n"
        "\n"
        "public class HeroController : IDamageable {\n"
        "    public int hp = 100;\n"
        "    public int Heal(int x) {\n"
        "        int e = FindEntity(\"PlayerHero\");\n"
        "        EmitSignal(\"on_shield_broken\");\n"
        "        int res = ResourceServer.Load(\"res://prefabs/Turret.prefab.txt\");\n"
        "        return ClampPositive(x + e + res);\n"
        "    }\n" +
        ifaceStubText +
        "}\n";

    lsp.service().update(combatPath, fixedCombatSrc, 2);
    const auto postFixDiags = lsp.service().diagnostics(combatPath);
    if (!postFixDiags.empty()) {
        std::string msg;
        for (const auto& d : postFixDiags) msg += d.code + ": " + d.message + "\n";
        throw std::runtime_error("Fixed combat.rscript produced diagnostics:\n" + msg);
    }

    // 5. Verify Scene Entity, Signal, and Resource string-literal completion & hover!
    // Line 12: `        int e = FindEntity("PlayerHero");` -> col 28 is right after `"Play`
    const auto entComp = lsp.service().completion(combatPath, {12, 32});
    require(std::any_of(entComp.begin(), entComp.end(), [](const auto& item) {
        return item.label == "PlayerHero" && item.detail.find("HeroController") != std::string::npos;
    }), "FindEntity(\"...\") completion did not suggest 'PlayerHero' with attached component metadata");

    const auto entPathComp = lsp.service().completion(combatPath, {12, 28}); // right at `"`
    require(std::any_of(entPathComp.begin(), entPathComp.end(), [](const auto& item) {
        return item.label == "/WorldRoot/PlayerHero";
    }), "FindEntity(\"...\") completion did not suggest hierarchical '/WorldRoot/PlayerHero' path");

    // Line 13: `        EmitSignal("on_shield_broken");` -> col 20 is right at `"`
    const auto sigComp = lsp.service().completion(combatPath, {13, 20});
    require(std::any_of(sigComp.begin(), sigComp.end(), [](const auto& item) {
        return item.label == "on_shield_broken";
    }) && std::any_of(sigComp.begin(), sigComp.end(), [](const auto& item) {
        return item.label == "on_boss_enraged";
    }), "EmitSignal(\"...\") completion did not merge .scene.txt and project.json signals");

    // Line 14: `        int res = ResourceServer.Load("res://prefabs/Turret.prefab.txt");` -> col 39 is right at `"`
    const auto resComp = lsp.service().completion(combatPath, {14, 39});
    require(std::any_of(resComp.begin(), resComp.end(), [](const auto& item) {
        return item.label == "res://prefabs/Turret.prefab.txt";
    }) && std::any_of(resComp.begin(), resComp.end(), [](const auto& item) {
        return item.label == "res://audio/boss_theme.ogg";
    }), "ResourceServer.Load(\"...\") completion did not list prefab & audio resources");

    // Hover on `"PlayerHero"` string literal (line 12, col 30)
    const auto entHover = lsp.service().hover(combatPath, {12, 30});
    require(entHover.has_value() &&
            entHover->contents.find("PlayerHero") != std::string::npos &&
            entHover->contents.find("/WorldRoot/PlayerHero") != std::string::npos &&
            entHover->contents.find("HeroController") != std::string::npos,
        "Hover on entity string literal missing scene entity details");

    // Hover on `IDamageable` (line 5, col 20) shows `[Guid=aaaaaaaa-...]`
    const auto guidHover = lsp.service().hover(combatPath, {5, 20});
    require(guidHover.has_value() &&
            guidHover->contents.find("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee") != std::string::npos,
        "Hover on [Guid]-annotated type did not display GUID");

    // 6. Verify `textDocument/prepareRename`:
    //    - Valid on user method `Heal` (line 11, col 17)
    //    - Rejected on keyword `public` (line 11, col 6)
    //    - Rejected on ambient C++ host function `FindEntity` (line 12, col 19)
    const auto validPrep = lsp.handle(tooling::Json::Object{
        {"jsonrpc", "2.0"}, {"id", 4},
        {"method", "textDocument/prepareRename"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{{"uri", combatUri}}},
            {"position", tooling::Json::Object{{"line", 11}, {"character", 17}}},
        }},
    });
    require(validPrep.find("result") &&
            validPrep.find("result")->find("placeholder")->stringValue() == "Heal",
        "prepareRename on user method 'Heal' failed");

    const auto kwPrep = lsp.handle(tooling::Json::Object{
        {"jsonrpc", "2.0"}, {"id", 5},
        {"method", "textDocument/prepareRename"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{{"uri", combatUri}}},
            {"position", tooling::Json::Object{{"line", 11}, {"character", 6}}},
        }},
    });
    require(kwPrep.find("error") != nullptr &&
            kwPrep.find("error")->find("message")->stringValue().find("keyword") != std::string::npos,
        "prepareRename must reject keywords with descriptive JSON-RPC error");

    const auto ambientPrep = lsp.handle(tooling::Json::Object{
        {"jsonrpc", "2.0"}, {"id", 6},
        {"method", "textDocument/prepareRename"},
        {"params", tooling::Json::Object{
            {"textDocument", tooling::Json::Object{{"uri", combatUri}}},
            {"position", tooling::Json::Object{{"line", 12}, {"character", 19}}},
        }},
    });
    require(ambientPrep.find("error") != nullptr &&
            ambientPrep.find("error")->find("message")->stringValue().find("ambient") != std::string::npos,
        "prepareRename must reject .d.rscript host ambient symbols");

    // 7. Verify `workspace/executeCommand` -> `realscript.refreshManifest`
    const auto refreshResp = lsp.handle(tooling::Json::Object{
        {"jsonrpc", "2.0"}, {"id", 7},
        {"method", "workspace/executeCommand"},
        {"params", tooling::Json::Object{
            {"command", "realscript.refreshManifest"},
        }},
    });
    require(refreshResp.find("result") &&
            refreshResp.find("result")->find("refreshed")->boolValue() &&
            refreshResp.find("result")->find("entities")->integerValue() >= 3,
        "workspace/executeCommand realscript.refreshManifest failed");

    std::filesystem::remove_all(tmpRoot, ec);
}

} // namespace

int main() {
    try {
        testDapEvaluatorAndPureGetterSandbox();
        testLspCodeActionsPrepareRenameAndSceneManifest();
        std::cout << "All P1 DAP evaluator and LSP scene manifest tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
