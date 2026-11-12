#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/debug/DapServer.h"
#include "realscript/game/Gameplay.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/tooling/LanguageService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef REALSCRIPT_SOURCE_DIR
#define REALSCRIPT_SOURCE_DIR \
    (std::filesystem::path(__FILE__).parent_path().parent_path().string())
#endif

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnosticsText(const diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (const auto& d : bag.items()) {
        out += "[" + d.code + "] " + d.message + "\n";
    }
    return out;
}

std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "failed to open file: " + path.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ============================================================================
// 1. Verify Hard Cutover to .rscript / .d.rscript in LanguageService (incl. open)
// ============================================================================
void testRscriptHardCutoverWorkspaceIndexing() {
    const auto nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto tempDir =
        std::filesystem::temp_directory_path() /
        ("sprint1_ws_tmp_" + std::to_string(nonce));
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    {
        std::ofstream rsRust(tempDir / "rust_module.rs", std::ios::binary);
        rsRust << "fn main() { let x: i32 = 42; }\n";

        std::ofstream oldStub(tempDir / "legacy.d.rs", std::ios::binary);
        oldStub << "module Legacy.Stub; int OldFn() { return 1; }\n";

        std::ofstream validStub(tempDir / "Engine.Stub.d.rscript", std::ios::binary);
        validStub << "module Engine.Stub;\npublic int HostMagic() { return 77; }\n";

        std::ofstream validScript(tempDir / "gameplay.rscript", std::ios::binary);
        validScript << "module Game.Test;\nimport Engine.Stub;\npublic int Run() { return HostMagic() + 1; }\n";
    }

    tooling::LanguageService service;
    const std::size_t indexed = service.addWorkspaceRoot(tempDir.generic_string());
    require(
        indexed == 2,
        "LanguageService hard cutover must index only .rscript and .d.rscript (expected 2, got " +
            std::to_string(indexed) + ")");

    // P3-1: LanguageService::open must strictly reject legacy .rs and .d.rs paths/URIs
    service.open("file:///tmp/legacy_rust.rs", "fn main() { invalid realscript syntax !!! }", 1);
    service.open("legacy_ambient.d.rs", "module Legacy.Ambient;\npublic int GhostFn() { return 9; }\n", 1);
    require(
        !service.contains("file:///tmp/legacy_rust.rs") &&
            !service.contains("legacy_ambient.d.rs") &&
            service.openDocumentPaths().empty(),
        "LanguageService::open must ignore .rs and .d.rs URIs/paths");

    service.open("file:///tmp/live_open.rscript", "module Game.Live;\npublic int Ping() { return 1; }\n", 1);
    service.open("live_stub.d.rscript", "module Game.LiveStub;\npublic int Stubbed() { return 2; }\n", 1);
    require(
        service.isOpenDocument("file:///tmp/live_open.rscript") &&
            service.isOpenDocument("live_stub.d.rscript") &&
            service.openDocumentPaths().size() == 2,
        "LanguageService::open must accept .rscript and .d.rscript URIs/paths");

    const auto scriptPath = (tempDir / "gameplay.rscript").generic_string();
    const auto diags = service.diagnostics(scriptPath);
    require(diags.empty(), "Valid .rscript + .d.rscript workspace produced diagnostics");

    std::filesystem::remove_all(tempDir, ec);
}

// ============================================================================
// 2. Verify Self-Contained Single-File C++ Declaration Parser (RsBindParser.h)
// ============================================================================
void testSingleFileCppDeclarationParser() {
    const char* engineHeader = R"CPP(
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#define ENGINE_API __declspec(dllexport)
#define DAMAGE_SCALAR int32_t

#if defined(PLATFORM_WIN64) && (ENGINE_API_LEVEL >= 3)
#define ACTIVE_COMBAT_V3 1
#else
#define ACTIVE_COMBAT_V3 0
#endif

namespace engine::combat {

using EntityHandleId = uint64_t;
typedef std::vector<int32_t> DamageHistory;

#if !ACTIVE_COMBAT_V3
RS_EXPORT_FUNCTION()
int DeadBranchShouldNeverExport(int x);
#endif

#if ENABLE_UNSET_EXPERIMENTAL_FEATURE
RS_EXPORT_FUNCTION()
int UnsetMacroMustEvaluateToZero(int x);
#endif

#ifdef ACTIVE_COMBAT_V3
RS_EXPORT_ENUM()
enum class BuffFlags : uint32_t {
    None = 0,
    Shielded = 1 << 0,
    Haste = 1 << 1,
    Berserk = (1 << 2) | 1,
};

RS_EXPORT_STRUCT("Game.Combat")
struct alignas(16) HitSample {
    Vec3 impactPoint{0, 0, 0};
    DAMAGE_SCALAR amount = 10;
    static constexpr int kVersion = 2;
    void ResetInlineHelper() { amount = 0; }
};

RS_EXPORT_CLASS("Game.Combat")
class ENGINE_API ActorBase {
public:
    RS_EXPORT_METHOD()
    [[nodiscard]] EntityHandleId GetEntityId() const noexcept { return id_; }
private:
    EntityHandleId id_ = 0;
};

RS_EXPORT_CLASS("Game.Combat")
class ENGINE_API BossUnit final : public ActorBase {
public:
    // Property declared BEFORE unexported getter to test AST two-pass return-type inference
    RS_EXPORT_PROPERTY("Rage", "GetRage", "SetRage")

    RS_EXPORT_CONSTRUCTOR()
    explicit BossUnit(int32_t tier, const std::string& title = "Boss")
        : tier_(tier), title_{title} {}

    float GetRage() const noexcept { return rage_; }
    void SetRage(float r) noexcept { rage_ = r; }

    RS_EXPORT_FIELD()
    const int32_t maxPhase = 3;

    RS_EXPORT_METHOD()
    [[nodiscard]] auto EvaluateThreat(
        const std::vector<std::shared_ptr<BossUnit>>& allies,
        BuffFlags flags = BuffFlags::None
    ) const noexcept -> DAMAGE_SCALAR;

    RS_EXPORT_METHOD()
    DamageHistory RecentHits() const;

private:
    int32_t tier_ = 1;
    std::string title_;
    float rage_ = 0.0f;
};
#endif

} // namespace engine::combat
)CPP";

    const std::unordered_map<std::string, std::string> macros = {
        {"PLATFORM_WIN64", "1"},
        {"ENGINE_API_LEVEL", "3"},
    };
    const auto gen = game::GameApi::generateBindingsFromCppHeader(
        engineHeader, "Game.Combat", macros);

    require(gen.functionsExported == 0, "Dead #if branches must be skipped by built-in preprocessor");
    require(gen.enumsExported == 1, "Expected 1 exported enum (BuffFlags)");
    require(gen.structsExported == 1, "Expected 1 exported ECS struct (HitSample)");
    require(gen.classesExported == 2, "Expected 2 exported classes (ActorBase, BossUnit)");
    require(gen.constructorsExported == 1, "Expected 1 exported constructor on BossUnit");
    require(gen.propertiesExported == 2, "Expected 2 exported properties (maxPhase read-only + Rage)");
    require(
        gen.scriptDeclarationStub.find("Berserk = 5") != std::string::npos,
        "Const-expr evaluator should resolve (1 << 2) | 1 to 5:\n" + gen.scriptDeclarationStub);
    require(
        gen.scriptDeclarationStub.find("class BossUnit : ActorBase") != std::string::npos,
        "Derived exported class stub should inherit ActorBase:\n" + gen.scriptDeclarationStub);
    require(
        gen.scriptDeclarationStub.find("float Rage") != std::string::npos,
        "Two-pass AST property inference should resolve GetRage() -> float:\n" + gen.scriptDeclarationStub);
    require(
        gen.scriptDeclarationStub.find("int EvaluateThreat(BossUnit[] allies, BuffFlags flags)") != std::string::npos,
        "Nested >> template, trailing return type, and macro typedef should resolve cleanly:\n" +
            gen.scriptDeclarationStub);
    require(
        gen.scriptDeclarationStub.find("int[] RecentHits()") != std::string::npos,
        "Typedef DamageHistory -> std::vector<int32_t> -> int[] should resolve:\n" +
            gen.scriptDeclarationStub);

    // Compile generated .d.rscript stub together with consumer .rscript
    game::GameApi emptyApi;
    game::GameScriptCompiler compiler(emptyApi);
    const char* consumerScript = R"RS(
module Game.Consumer;
import Game.Combat;

public int VerifyBindings(BossUnit boss, BossUnit[] team) {
    boss.Rage = (float)2.5;
    int threat = boss.EvaluateThreat(team, BuffFlags.Berserk);
    ulong eid = boss.GetEntityId();
    return threat + boss.maxPhase + (int)eid;
}
)RS";
    const auto compiled = compiler.compile({
        {"Game.Combat.d.rscript", gen.scriptDeclarationStub},
        {"consumer.rscript", consumerScript},
    });
    require(
        compiled.succeeded(),
        "Generated .d.rscript stub + consumer failed to compile:\n" +
            diagnosticsText(compiled.diagnostics));

    // P1-1 Regression Suite:
    // - True #if short-circuit (`1 || __has_feature(cxx_rtti)`, `0 && (1 / 0)`, `1 ? 7 : (1 / 0)`)
    // - INT64_MIN / -1 and INT64_MIN % -1 SIGFPE guard + `1ULL << 63` unsigned shift
    // - Empty function-like macro `#define DEPRECATED(msg)` swallowing `(args)`
    // - `operator<`, `operator<<`, `operator<=`, `operator->`, `operator()` not corrupting angleDepth
    // - Multi-declarator fields `int x, y;`
    // - Namespace-qualified overload `static_cast<ns::Mode (ns::Gadget::*)(ns::Mode)>`
    // - Cross-namespace same-name classes (`alpha::Gadget` vs `beta::Gadget`)
    // - Reject ECS struct with unmapped field type (never silently emit 4-byte Int32)
    // - Custom module annotation respected in `.d.rscript` generation
    const char* trickyHeader = R"CPP(
#pragma once
#define DEPRECATED(msg)
#define ADD_ONE(v) ((v) + 1)

#if (1 || __has_feature(cxx_rtti)) && !(0 && (1 / 0)) && ((1 ? 9 : (1 / 0)) == 9)
#if ((-9223372036854775807LL - 1) / -1 != 0) && ((-9223372036854775807LL - 1) % -1 == 0) && ((1ULL << 63) != 0ULL)

namespace alpha {

RS_EXPORT_ENUM("Custom.Alpha")
enum class Mode : int32_t {
    Idle = 0,
    Turbo = ADD_ONE(2),
};

struct OpaqueUnmappedBlob {
    void* rawPtr;
};

RS_EXPORT_STRUCT("Custom.Alpha")
struct InvalidEcsStruct {
    int32_t validId;
    OpaqueUnmappedBlob brokenField;
};

RS_EXPORT_STRUCT("Custom.Alpha")
struct ValidPair {
    int32_t cx, cy;
};

RS_EXPORT_CLASS("Custom.Alpha")
class Gadget {
public:
    bool operator<(const Gadget& rhs) const noexcept { return a < rhs.a; }
    bool operator<=(const Gadget& rhs) const noexcept { return a <= rhs.a; }
    Gadget& operator<<(int shift) noexcept { a <<= shift; return *this; }
    const Gadget* operator->() const noexcept { return this; }
    int operator()(int k) const noexcept { return a + k; }

    RS_EXPORT_FIELD()
    int32_t a, b;

    DEPRECATED("legacy overload")
    RS_EXPORT_METHOD()
    Mode Transform(Mode input) const;

    RS_EXPORT_METHOD()
    Mode Transform(Mode input, int32_t extra) const;
};

DEPRECATED("use v2")
RS_EXPORT_FUNCTION("CustomAlphaTop")
int32_t GlobalPing(int32_t x);

} // namespace alpha

namespace beta {

RS_EXPORT_CLASS("Custom.Alpha")
class Gadget {
public:
    RS_EXPORT_METHOD()
    int32_t BetaPing() const;
};

} // namespace beta

#endif
#endif
)CPP";

    const auto trickyGen = game::GameApi::generateBindingsFromCppHeader(
        trickyHeader, "Custom.Alpha");
    require(
        trickyGen.classesExported == 2,
        "Short-circuit #if, INT64_MIN/-1 guard, 1ULL<<63, and same-name cross-namespace classes must export 2 classes");
    require(
        trickyGen.structsExported == 1,
        "InvalidEcsStruct with unmapped field must be rejected while ValidPair(cx, cy) exports (expected 1, got " +
            std::to_string(trickyGen.structsExported) + ")");
    require(
        trickyGen.cppRegistrationSource.find("registerStruct<alpha::InvalidEcsStruct>") == std::string::npos &&
            trickyGen.scriptDeclarationStub.find("struct InvalidEcsStruct") == std::string::npos,
        "Unmapped ECS struct must not silently fall back to Int32 field layout:\n" + trickyGen.cppRegistrationSource);
    require(
        trickyGen.cppRegistrationSource.find("builder_alpha_Gadget") != std::string::npos &&
            trickyGen.cppRegistrationSource.find("builder_beta_Gadget") != std::string::npos,
        "Cross-namespace same-name classes must generate distinct builder_<ns_Cls> variables:\n" +
            trickyGen.cppRegistrationSource);
    require(
        trickyGen.cppRegistrationSource.find("static_cast<alpha::Mode (alpha::Gadget::*)(alpha::Mode) const>(&alpha::Gadget::Transform)") !=
            std::string::npos,
        "Overloaded member static_cast at global scope must qualify namespace-local return & parameter types:\n" +
            trickyGen.cppRegistrationSource);
    require(
        trickyGen.scriptDeclarationStub.find("module Custom.Alpha;") != std::string::npos &&
            trickyGen.scriptDeclarationStub.find("int a") != std::string::npos &&
            trickyGen.scriptDeclarationStub.find("int b") != std::string::npos &&
            trickyGen.scriptDeclarationStub.find("Mode Transform(Mode input)") != std::string::npos,
        "operator</<< must not swallow subsequent methods, multi-declarator int32_t a, b must split, and DEPRECATED(msg) must be stripped:\n" +
            trickyGen.scriptDeclarationStub);

    // Also verify custom module annotation overrides defaultModuleName when generating .d.rscript
    const char* customModHeader = R"CPP(
#define DEPRECATED(msg)
DEPRECATED("legacy")
RS_EXPORT_FUNCTION("CustomAlphaTop")
int32_t GlobalPing(int32_t x);
)CPP";
    const auto customModGen = game::GameApi::generateBindingsFromCppHeader(
        customModHeader, "Fallback.Default");
    require(
        customModGen.functionsExported == 1 &&
            customModGen.cppRegistrationSource.find("\"CustomAlphaTop\"") != std::string::npos &&
            customModGen.scriptDeclarationStub.find("module CustomAlphaTop;") != std::string::npos &&
            customModGen.scriptDeclarationStub.find("GlobalPing(int x)") != std::string::npos,
        "RS_EXPORT_FUNCTION(\"CustomAlphaTop\") without dot must preserve custom module name:\n" +
            customModGen.scriptDeclarationStub);

    // Exponential macro expansion cap test (must return promptly without hanging)
    const char* macroBombHeader = R"CPP(
#define B0 B1 B1 B1 B1
#define B1 B2 B2 B2 B2
#define B2 B3 B3 B3 B3
#define B3 B4 B4 B4 B4
#define B4 B5 B5 B5 B5
#define B5 B6 B6 B6 B6
#define B6 B7 B7 B7 B7
#define B7 B8 B8 B8 B8
#define B8 1 +
#if B0 0
#endif
RS_EXPORT_FUNCTION("Safe.Mod")
int AfterBomb(int v);
)CPP";
    const auto bombGen = game::GameApi::generateBindingsFromCppHeader(macroBombHeader, "Safe.Mod");
    require(bombGen.functionsExported == 1, "Macro step cap must prevent hang and continue parsing header");
}

// ============================================================================
// 3. Verify Zero-Allocation Struct Enumerators, Zero-Alloc foreach, Indexers & O(1) Remove
// ============================================================================
void testZeroAllocContainersAndO1Remove() {
    const char* src = R"RS(
module Sprint1.Containers;

public int SetupAndVerifySemantics() {
    List<int> list = new List<int>(8);
    list.Add(10);
    list.Add(20);
    list.Add(30);
    list[1] = 25;
    list[2] += 5;
    if (list.Count != 3 || list.Capacity < 8 || list.IsEmpty) return -1;
    if (list[0] != 10 || list[1] != 25 || list[2] != 35) return -2;

    // Manual GetEnumerator() value-type struct check
    var listEnum = list.GetEnumerator();
    int manualSum = 0;
    while (listEnum.MoveNext()) {
        manualSum += listEnum.Current();
    }
    if (manualSum != 70) return -3;

    // Stress Dictionary & HashSet O(1) Swap-and-Pop Remove across 256 items
    Dictionary<int, int> dict = new Dictionary<int, int>(8);
    HashSet<int> hs = new HashSet<int>(8);
    for (int i = 0; i < 256; i = i + 1) {
        dict[i] = i * 10;
        hs.Add(i);
    }
    // Remove even keys (interleaved across colliding buckets)
    for (int i = 0; i < 256; i = i + 2) {
        if (!dict.Remove(i)) return -4;
        if (!hs.Remove(i)) return -5;
    }
    if (dict.Count != 128 || hs.Count != 128) return -6;
    for (int i = 0; i < 256; i = i + 1) {
        if ((i % 2) == 0) {
            if (dict.ContainsKey(i) || hs.Contains(i)) return -7;
        } else {
            if (!dict.ContainsKey(i) || dict[i] != i * 10 || !hs.Contains(i)) return -8;
            dict[i] += 1;
        }
    }

    int capBeforeClear = dict.Capacity;
    dict.Clear();
    hs.Clear();
    if (!dict.IsEmpty || !hs.IsEmpty || dict.Capacity != capBeforeClear) return -9;

    return 70;
}

class Holder {
    public List<int> list;
    public Queue<int> queue;
    public Stack<int> stack;
    public HashSet<int> hs;
    public static Holder Instance;
    public Dictionary<string, int> dict;

    public Holder() {
        list = new List<int>(8);
        list.Add(1); list.Add(2); list.Add(3);

        queue = new Queue<int>(8);
        queue.Enqueue(4); queue.Enqueue(5);

        stack = new Stack<int>(8);
        stack.Push(6); stack.Push(7);

        hs = new HashSet<int>(8);
        hs.Add(8); hs.Add(9);

        dict = new Dictionary<string, int>(8);
        dict["a"] = 10;
        dict["b"] = 11;
    }

    public int SumAllForeach() {
        int total = 0;
        for (int iter = 0; iter < 100; iter = iter + 1) {
            foreach (int v in list) total += v;
            foreach (int v in queue) total += v;
            foreach (int v in stack) total += v;
            foreach (int v in hs) total += v;
            foreach (int v in dict) total += v;
        }
        return total;
    }
}

public int InitHolder() {
    Holder.Instance = new Holder();
    return 1;
}

public int HotLoopForeach() {
    return Holder.Instance.SumAllForeach();
}
)RS";

    compiler::Compilation compilation({{"sprint1_containers.rscript", src}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(), "Container test compile failed:\n" + diagnosticsText(build.diagnostics));

    // Verify Enumerator types were emitted as `TypeKind::Struct` (not Heap Class!)
    bool foundStructEnumerator = false;
    for (const auto& mod : build.modules) {
        for (const auto& t : mod.types) {
            if (t.name.rfind("__RsRef__", 0) == 0) {
                continue;
            }
            if (t.name.find("__Enumerator") != std::string::npos) {
                require(
                    t.kind == semantic::TypeKind::Struct,
                    "Built-in collection enumerator '" + t.name + "' must be a value-type struct");
                foundStructEnumerator = true;
            }
        }
    }
    require(foundStructEnumerator, "Expected synthesized __Enumerator struct types in module");

    optimization::Options optOpts;
    optOpts.level = optimization::Level::Aggressive;
    auto optimized = optimization::Optimizer{}.optimize(std::move(build.modules), build.diagnostics, optOpts);

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcModules;
    for (const auto& m : optimized.modules) bcModules.push_back(lowerer.lower(m));

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcModules), linkErr);
    require(linked.has_value(), "ProgramImage::link failed: " + linkErr.message);
    auto progImg = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::Interpreter interp(progImg, heap);

    const auto semRes = interp.invoke("Sprint1.Containers::SetupAndVerifySemantics");
    require(
        semRes.succeeded && std::get<std::int64_t>(semRes.value) == 70,
        "SetupAndVerifySemantics failed: " +
            (semRes.succeeded ? std::to_string(std::get<std::int64_t>(semRes.value)) : semRes.error.message));

    const auto initRes = interp.invoke("Sprint1.Containers::InitHolder");
    require(initRes.succeeded, "InitHolder failed");

    const auto liveBefore = heap->liveObjects();
    const auto bytesBefore = heap->liveBytes();
    const auto hotRes = interp.invoke("Sprint1.Containers::HotLoopForeach");
    require(
        hotRes.succeeded && std::get<std::int64_t>(hotRes.value) == 6600,
        "HotLoopForeach returned unexpected sum");
    const auto liveAfter = heap->liveObjects();
    const auto bytesAfter = heap->liveBytes();
    require(
        liveAfter == liveBefore && bytesAfter == bytesBefore,
        "500 container foreach loops must allocate zero objects on ManagedHeap even before GC");
}

// ============================================================================
// 4. Verify DAP Watch `evaluate`, Queue/Stack/Dictionary/HashSet Views,
//    Retaining Path Outer ObjectRef, nextChildRef >= 1000000 & Natvis/LLDB
// ============================================================================
void testDapWatchTreeExpansionAndNatvis() {
    game::GameApi api;
    const char* dapScript = R"RS(
module Sprint1.DapWatch;
import RealScript.Math;

struct Stats {
    public int atk;
    public int def;
    public Stats(int a, int d) { atk = a; def = d; }
}

class Hero {
    public string name = "Arthas";
    public Stats stats;
    public Vec3 pos;
    public List<int> loot;
    public Dictionary<string, int> bag;
    public Queue<int> turnQueue;
    public Stack<int> undoStack;
    public HashSet<int> badges;

    public Hero() {
        stats = new Stats(88, 44);
        pos = Vec3.Of(3, 4, 5);
        loot = new List<int>(4);
        loot.Add(101);
        loot.Add(202);
        bag = new Dictionary<string, int>(4);
        bag["gold"] = 999;

        // Wrap-around ring buffer: cap=4, enqueue 3, dequeue 2 (head=2), enqueue 3 more -> logical [30, 40, 50]
        turnQueue = new Queue<int>(4);
        turnQueue.Enqueue(10);
        turnQueue.Enqueue(20);
        turnQueue.Enqueue(30);
        int d1 = turnQueue.Dequeue();
        int d2 = turnQueue.Dequeue();
        turnQueue.Enqueue(40 + d1 * 0 + d2 * 0);
        turnQueue.Enqueue(50);

        undoStack = new Stack<int>(4);
        undoStack.Push(11);
        undoStack.Push(22);
        undoStack.Push(33);

        badges = new HashSet<int>(4);
        badges.Add(7);
    }
}

public int DebugEntry() {
    Hero hero = new Hero();
    int marker = hero.stats.atk;
    return marker + hero.bag["gold"];
}
)RS";

    game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"sprint1_dap.rscript", dapScript}});
    require(compiled.succeeded(), "DAP Watch script failed to compile:\n" + diagnosticsText(compiled.diagnostics));

    debug::DapServer dap(compiled.program.program(), compiled.program.heap());
    std::ostringstream events;
    (void)dap.handle(tooling::Json::Object{
        {"seq", 1},
        {"type", "request"},
        {"command", "setBreakpoints"},
        {"arguments", tooling::Json::Object{
            {"source", tooling::Json::Object{{"path", "sprint1_dap.rscript"}}},
            {"breakpoints", tooling::Json::Array{
                tooling::Json::Object{{"line", 52}, {"column", 1}},
            }},
        }},
    }, &events);

    (void)dap.handle(tooling::Json::Object{
        {"seq", 2},
        {"type", "request"},
        {"command", "launch"},
        {"arguments", tooling::Json::Object{
            {"function", "Sprint1.DapWatch::DebugEntry"},
        }},
    }, &events);

    // 1. Evaluate `hero` in Watch — must return variablesReference >= 1000000 (P1-8)!
    const auto evalHero = dap.handle(tooling::Json::Object{
        {"seq", 3},
        {"type", "request"},
        {"command", "evaluate"},
        {"arguments", tooling::Json::Object{
            {"expression", "hero"},
            {"frameId", 1},
        }},
    }, &events);
    require(evalHero.find("success")->boolValue(), "Watch evaluate 'hero' failed");
    const auto* heroBody = evalHero.find("body");
    const std::int64_t heroRef = heroBody->find("variablesReference")->integerValue();
    require(
        heroRef >= debug::DapServer::kFirstChildVariablesReference,
        "Watch evaluate 'hero' must return variablesReference >= 1000000 to avoid deep frame.id*2 collisions");
    require(heroBody->find("type")->stringValue() == "Hero", "Watch evaluate 'hero' type should be 'Hero'");

    // 2. Expand `hero` children from Watch variablesReference
    const auto heroVars = dap.handle(tooling::Json::Object{
        {"seq", 4},
        {"type", "request"},
        {"command", "variables"},
        {"arguments", tooling::Json::Object{{"variablesReference", heroRef}}},
    }, &events);
    const auto* heroChildren = heroVars.find("body")->find("variables");
    require(heroChildren && heroChildren->isArray() && heroChildren->arrayValue().size() == 8,
        "Expanding watched 'hero' should yield 8 fields");

    // 3. Verify Queue<int> logical head-to-tail ring-buffer order ([0]=30, [1]=40, [2]=50) & outer RetainingPath!
    const auto evalQueue = dap.handle(tooling::Json::Object{
        {"seq", 5},
        {"type", "request"},
        {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", "hero.turnQueue"}, {"frameId", 1}}},
    }, &events);
    const std::int64_t queueRef = evalQueue.find("body")->find("variablesReference")->integerValue();
    require(queueRef >= debug::DapServer::kFirstChildVariablesReference, "Queue must be expandable");
    const auto queueVars = dap.handle(tooling::Json::Object{
        {"seq", 6},
        {"type", "request"},
        {"command", "variables"},
        {"arguments", tooling::Json::Object{{"variablesReference", queueRef}}},
    }, &events);
    const auto& qArr = queueVars.find("body")->find("variables")->arrayValue();
    require(
        qArr.size() == 4 &&
            qArr[0].find("name")->stringValue() == "Count" &&
            qArr[0].find("value")->stringValue() == "3" &&
            qArr[1].find("name")->stringValue() == "[0]" &&
            qArr[1].find("value")->stringValue() == "30" &&
            qArr[2].find("name")->stringValue() == "[1]" &&
            qArr[2].find("value")->stringValue() == "40" &&
            qArr[3].find("name")->stringValue() == "[2]" &&
            qArr[3].find("value")->stringValue() == "50",
        "Queue<int> DAP preview must unwrap circular ring buffer from head to tail");

    const auto queueRetaining = dap.handle(tooling::Json::Object{
        {"seq", 7},
        {"type", "request"},
        {"command", "retainingPath"},
        {"arguments", tooling::Json::Object{{"variablesReference", queueRef}}},
    }, &events);
    require(queueRetaining.find("success")->boolValue(), "retainingPath on Queue variablesReference failed");
    const std::string qPathText = queueRetaining.find("body")->find("retainingPath")->stringValue();
    require(
        qPathText.find("Queue__int") != std::string::npos &&
            qPathText.find(".items") == std::string::npos,
        "Collection childOwnerObjects_ must retain outer Queue ObjectRef (not backing items array): " + qPathText);

    // 4. Verify Stack<int> top-first reverse order ([0]=33, [1]=22, [2]=11)!
    const auto evalStack = dap.handle(tooling::Json::Object{
        {"seq", 8},
        {"type", "request"},
        {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", "hero.undoStack"}, {"frameId", 1}}},
    }, &events);
    const std::int64_t stackRef = evalStack.find("body")->find("variablesReference")->integerValue();
    const auto stackVars = dap.handle(tooling::Json::Object{
        {"seq", 9},
        {"type", "request"},
        {"command", "variables"},
        {"arguments", tooling::Json::Object{{"variablesReference", stackRef}}},
    }, &events);
    const auto& sArr = stackVars.find("body")->find("variables")->arrayValue();
    require(
        sArr.size() == 4 &&
            sArr[0].find("name")->stringValue() == "Count" &&
            sArr[1].find("value")->stringValue() == "33" &&
            sArr[2].find("value")->stringValue() == "22" &&
            sArr[3].find("value")->stringValue() == "11",
        "Stack<int> DAP preview must display top-of-stack first");

    // 5. Verify Dictionary<string, int> view + in-place setVariable on [gold], and HashSet setVariable rejection!
    const auto evalBag = dap.handle(tooling::Json::Object{
        {"seq", 10},
        {"type", "request"},
        {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", "hero.bag"}, {"frameId", 1}}},
    }, &events);
    const std::int64_t bagRef = evalBag.find("body")->find("variablesReference")->integerValue();
    require(bagRef >= debug::DapServer::kFirstChildVariablesReference, "Watch evaluate 'hero.bag' must be expandable");
    const auto setDictRes = dap.handle(tooling::Json::Object{
        {"seq", 11},
        {"type", "request"},
        {"command", "setVariable"},
        {"arguments", tooling::Json::Object{
            {"variablesReference", bagRef},
            {"name", "[gold]"},
            {"value", "1500"},
        }},
    }, &events);
    require(
        setDictRes.find("success")->boolValue() &&
            setDictRes.find("body")->find("value")->stringValue() == "1500",
        "setVariable on Dictionary [key] must update value slot in-place");
    const auto reEvalBag = dap.handle(tooling::Json::Object{
        {"seq", 12},
        {"type", "request"},
        {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", "hero.bag"}, {"frameId", 1}}},
    }, &events);
    const std::int64_t freshBagRef = reEvalBag.find("body")->find("variablesReference")->integerValue();
    const auto verifyBagVars = dap.handle(tooling::Json::Object{
        {"seq", 120},
        {"type", "request"},
        {"command", "variables"},
        {"arguments", tooling::Json::Object{{"variablesReference", freshBagRef}}},
    }, &events);
    const auto& freshBagChildren = verifyBagVars.find("body")->find("variables")->arrayValue();
    require(
        freshBagChildren.size() == 2 &&
            freshBagChildren[1].find("name")->stringValue() == "[gold]" &&
            freshBagChildren[1].find("value")->stringValue() == "1500",
        "Mutated Dictionary [gold] value must persist in live heap");

    const auto evalBadges = dap.handle(tooling::Json::Object{
        {"seq", 13},
        {"type", "request"},
        {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", "hero.badges"}, {"frameId", 1}}},
    }, &events);
    const std::int64_t badgesRef = evalBadges.find("body")->find("variablesReference")->integerValue();
    const auto setHashSetRes = dap.handle(tooling::Json::Object{
        {"seq", 14},
        {"type", "request"},
        {"command", "setVariable"},
        {"arguments", tooling::Json::Object{
            {"variablesReference", badgesRef},
            {"name", "[0]"},
            {"value", "99"},
        }},
    }, &events);
    require(
        !setHashSetRes.find("success")->boolValue(),
        "setVariable on HashSet element must fail with explicit error to prevent bucket chain corruption");

    (void)dap.handle(tooling::Json::Object{
        {"seq", 15},
        {"type", "request"},
        {"command", "continue"},
    }, &events);

    // 6. Verify Natvis & LLDB files exist, include P3-1 fixes, and AOT struct codegen includes field comments
    const std::filesystem::path root(REALSCRIPT_SOURCE_DIR);
    const auto natvisPath = root / "tools/debuggers/RealScript.natvis";
    const auto lldbPath = root / "tools/debuggers/realscript_lldb.py";
    require(std::filesystem::exists(natvisPath), "Missing RealScript.natvis");
    require(std::filesystem::exists(lldbPath), "Missing realscript_lldb.py");

    const std::string natvisText = readWholeFile(natvisPath);
    require(
        natvisText.find("heapId == 0") != std::string::npos &&
            natvisText.find("version == 0") != std::string::npos &&
            natvisText.find("signW") != std::string::npos,
        "RealScript.natvis must check heapId == 0, version == 0, and display QuatInline signW");

    const std::string lldbText = readWholeFile(lldbPath);
    require(
        lldbText.find("vtag == 21") != std::string::npos &&
            lldbText.find("vtag == 22") != std::string::npos &&
            lldbText.find("vtag == 23") != std::string::npos &&
            lldbText.find("vtag == 24") != std::string::npos &&
            lldbText.find("hid == 0") != std::string::npos,
        "realscript_lldb.py must unpack vtag 21..24 and check ObjectRef hid == 0");

    compiler::Compilation aotComp({{"struct_dbg.rscript", R"RS(
module Sprint1.AotDbg;
struct DamagePacket {
    public int baseHit;
    public int critBonus;
}
public int Compute(int x) {
    DamagePacket p = new DamagePacket();
    p.baseHit = x;
    p.critBonus = 5;
    return p.baseHit + p.critBonus;
}
)RS"}});
    auto aotBuild = aotComp.build();
    aot::CppGenerator gen;
    aot::GenerationOptions genOpts;
    genOpts.programName = "Sprint1Dbg";
    const auto emitted = gen.generate(aotBuild.modules, aotBuild.diagnostics, genOpts);
    require(
        emitted.source.find("// RealScript struct: Sprint1.AotDbg::DamagePacket") != std::string::npos &&
            emitted.source.find("// baseHit") != std::string::npos &&
            emitted.source.find("// critBonus") != std::string::npos,
        "AOT generated struct should include RealScript type and field debug comments");
}

} // namespace

int main() {
    try {
        std::cout << "[1/4] testRscriptHardCutoverWorkspaceIndexing...\n" << std::flush;
        testRscriptHardCutoverWorkspaceIndexing();
        std::cout << "[2/4] testSingleFileCppDeclarationParser...\n" << std::flush;
        testSingleFileCppDeclarationParser();
        std::cout << "[3/4] testZeroAllocContainersAndO1Remove...\n" << std::flush;
        testZeroAllocContainersAndO1Remove();
        std::cout << "[4/4] testDapWatchTreeExpansionAndNatvis...\n" << std::flush;
        testDapWatchTreeExpansionAndNatvis();
        std::cout << "Sprint 1 evolution conformance tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
