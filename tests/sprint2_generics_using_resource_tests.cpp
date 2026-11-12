#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/Gameplay.h"
#include "realscript/game/ResourceHandle.h"
#include "realscript/jit/Jit.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/text/Text.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnosticsText(const realscript::diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (const auto& d : bag.items()) {
        out += "[" + d.sourceName + "] " + d.code + ": " + d.message + "\n";
    }
    return out;
}

bool hasDiagnosticCode(
    const realscript::diagnostics::DiagnosticBag& bag,
    const std::string& code) {
    for (const auto& d : bag.items()) {
        if (d.code == code) return true;
    }
    return false;
}

realscript::runtime::Interpreter buildInterpreter(
    const realscript::compiler::BuildResult& build,
    std::shared_ptr<realscript::runtime::ManagedHeap> heap = nullptr,
    std::shared_ptr<const realscript::runtime::BindingRegistry> bindings = nullptr) {
    require(!build.diagnostics.hasErrors(),
        "Compilation failed unexpectedly:\n" + diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> lowered;
    lowered.reserve(build.modules.size());
    for (const auto& mod : build.modules) {
        auto bc = lowerer.lower(mod);
        realscript::diagnostics::DiagnosticBag verifyBag;
        require(realscript::bytecode::verifyModule(bc, verifyBag),
            "Bytecode verification failed:\n" + diagnosticsText(verifyBag));
        lowered.push_back(std::move(bc));
    }
    realscript::runtime::Interpreter interp(std::move(lowered));
    if (heap) interp.setHeap(std::move(heap));
    if (bindings) interp.setBindingRegistry(std::move(bindings));
    return interp;
}

std::string readEnvString(const char* name) {
    if (name == nullptr || name[0] == '\0') return {};
#if defined(_MSC_VER)
    char* rawValue = nullptr;
    std::size_t rawLen = 0;
    if (_dupenv_s(&rawValue, &rawLen, name) != 0 || rawValue == nullptr) {
        return {};
    }
    std::string result(rawValue);
    std::free(rawValue);
    return result;
#else
    const char* rawValue = std::getenv(name);
    return rawValue ? std::string(rawValue) : std::string{};
#endif
}

std::filesystem::path findExecutableOnPath(const std::string& rawName) {
    if (rawName.empty()) return {};
    std::error_code ec;
    const std::filesystem::path direct(rawName);
    if (direct.has_parent_path() && std::filesystem::exists(direct, ec)) {
        return direct;
    }
    const std::string pathEnv = readEnvString("PATH");
    if (pathEnv.empty()) return {};
#if defined(_WIN32)
    const char delimiter = ';';
    const std::vector<std::string> suffixes = {"", ".exe", ".cmd", ".bat"};
#else
    const char delimiter = ':';
    const std::vector<std::string> suffixes = {""};
#endif
    std::stringstream ss(pathEnv);
    std::string entry;
    while (std::getline(ss, entry, delimiter)) {
        if (entry.empty()) continue;
        const std::filesystem::path dir(entry);
        for (const auto& suffix : suffixes) {
            const auto candidate = dir / (rawName + suffix);
            if (std::filesystem::exists(candidate, ec) &&
                !std::filesystem::is_directory(candidate, ec)) {
                return candidate;
            }
        }
    }
    return {};
}

std::filesystem::path resolveDefaultCompilerPath() {
#if defined(REALSCRIPT_JIT_COMPILER)
    {
        const auto configured = findExecutableOnPath(REALSCRIPT_JIT_COMPILER);
        if (!configured.empty()) return configured;
        std::error_code ec;
        if (std::filesystem::exists(REALSCRIPT_JIT_COMPILER, ec)) {
            return std::filesystem::path(REALSCRIPT_JIT_COMPILER);
        }
    }
#endif
    if (const std::string cxxEnv = readEnvString("CXX"); !cxxEnv.empty()) {
        const auto fromEnv = findExecutableOnPath(cxxEnv);
        if (!fromEnv.empty()) return fromEnv;
    }
#if defined(_WIN32)
    for (const char* name : {"cl", "clang-cl", "clang++", "g++"}) {
#else
    for (const char* name : {"g++", "clang++", "c++"}) {
#endif
        const auto found = findExecutableOnPath(name);
        if (!found.empty()) return found;
    }
    return {};
}

std::filesystem::path resolveDefaultIncludeDirectory() {
#if defined(REALSCRIPT_JIT_INCLUDE_DIR)
    return std::filesystem::path(REALSCRIPT_JIT_INCLUDE_DIR);
#elif defined(REALSCRIPT_SOURCE_DIR)
    return std::filesystem::path(REALSCRIPT_SOURCE_DIR) / "include";
#else
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "include";
#endif
}

std::filesystem::path resolveDefaultSupportLibrary() {
#if defined(REALSCRIPT_JIT_SUPPORT_LIBRARY)
    return std::filesystem::path(REALSCRIPT_JIT_SUPPORT_LIBRARY);
#elif defined(REALSCRIPT_SUPPORT_LIBRARY)
    return std::filesystem::path(REALSCRIPT_SUPPORT_LIBRARY);
#else
    std::error_code ec;
    for (const char* candidate : {
             "librealscript_frontend.a",
             "realscript_frontend.lib",
             "Debug/realscript_frontend.lib",
             "Release/realscript_frontend.lib"}) {
        const std::filesystem::path p(candidate);
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
#endif
}

std::filesystem::path resolveDefaultCacheBaseDir() {
    if (const std::string aotTmpEnv = readEnvString("RS_AOT_TMPDIR"); !aotTmpEnv.empty()) {
        return std::filesystem::path(aotTmpEnv);
    }
#if defined(REALSCRIPT_JIT_CACHE_DIR)
    return std::filesystem::path(REALSCRIPT_JIT_CACHE_DIR);
#else
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec);
    return ec ? std::filesystem::path("jit-sprint2-cache") : (tmp / "realscript_sprint2_jit_cache");
#endif
}

bool compilerLooksLikeMsvc(const std::filesystem::path& compilerPath) {
    auto stem = compilerPath.stem().string();
    for (auto& ch : stem) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return stem == "cl" || stem == "clang-cl";
}

struct CompiledAotLibrary {
    std::shared_ptr<realscript::jit::Module> module;
    std::filesystem::path outputDir;
    const realscript::aot::ProgramDescriptor* descriptor = nullptr;

    ~CompiledAotLibrary() {
        descriptor = nullptr;
        module.reset();
        if (!outputDir.empty()) {
            std::error_code rmEc;
            std::filesystem::remove_all(outputDir, rmEc);
        }
    }
};

std::unique_ptr<CompiledAotLibrary> compileAndLoadAot(
    const std::vector<realscript::mir::Module>& modules,
    const realscript::aot::GenerationOptions& genOpts,
    realscript::optimization::Level optLevel = realscript::optimization::Level::None) {
    static std::atomic<int> counter{0};
    const int seq = counter.fetch_add(1) + 1;
    const auto stamp = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());

    realscript::jit::ToolchainOptions options;
    options.compiler = resolveDefaultCompilerPath();
    options.includeDirectory = resolveDefaultIncludeDirectory();
    options.supportLibrary = resolveDefaultSupportLibrary();
    options.outputDirectory = resolveDefaultCacheBaseDir() /
        (genOpts.programName + "_" + std::to_string(stamp) + "_" + std::to_string(seq));
    options.generation = genOpts;
    options.optimization.level = optLevel;
    options.reuseCachedLibrary = false;
    options.keepGeneratedSources = false;

    if (compilerLooksLikeMsvc(options.compiler)) {
#if defined(_DEBUG)
        options.compilerArguments.push_back("/MDd");
#else
        options.compilerArguments.push_back("/MD");
#endif
        options.compilerArguments.push_back("/W4");
        options.compilerArguments.push_back("/WX");
    } else {
        options.compilerArguments.push_back("-O2");
        options.compilerArguments.push_back("-Wall");
        options.compilerArguments.push_back("-Wextra");
        options.compilerArguments.push_back("-Wpedantic");
        options.compilerArguments.push_back("-Werror");
    }

    require(realscript::jit::ToolchainJit::available(options),
        "ToolchainJit is unavailable (compiler='" + options.compiler.string() +
            "', include='" + options.includeDirectory.string() +
            "', support='" + options.supportLibrary.string() + "')");

    realscript::jit::ToolchainJit jit;
    auto compiled = jit.compile(modules, options);
    require(compiled.succeeded(),
        "ToolchainJit AOT compilation failed: " + compiled.error + "\n" +
            compiled.compilerOutput);

    auto lib = std::make_unique<CompiledAotLibrary>();
    lib->outputDir = options.outputDirectory;
    lib->module = std::move(compiled.module);
    lib->descriptor = lib->module->descriptor();
    require(lib->descriptor != nullptr, "ProgramDescriptor pointer from ToolchainJit is null");
    return lib;
}

// 1. lang_3(1): Exact SourceSpan (file, line, column) preservation inside generic templates
void testGenericDiagnosticExactLineNumbers() {
    const std::string templateSource =
        "module Sprint2.DiagLib;\n"                        // line 1
        "\n"                                               // line 2
        "// Header comment padding to catch line drift\n"  // line 3
        "// Line 4\n"                                      // line 4
        "// Line 5\n"                                      // line 5
        "public class Payload {\n"                         // line 6
        "    public int count;\n"                          // line 7
        "}\n"                                              // line 8
        "\n"                                               // line 9
        "public class GenericBox<T> {\n"                   // line 10
        "    T item;\n"                                    // line 11
        "\n"                                               // line 12
        "    public GenericBox(T v) {\n"                   // line 13
        "        item = v;\n"                              // line 14
        "    }\n"                                          // line 15
        "\n"                                               // line 16
        "    public int BrokenMethod() {\n"                // line 17
        "        int ok = 42;\n"                           // line 18
        "        int badAssign = \"type_mismatch_line19\";\n" // line 19
        "        return ok + badAssign;\n"                 // line 20
        "    }\n"                                          // line 21
        "}\n"                                              // line 22
        "\n"                                               // line 23
        "public int GenericFreeBroken<T>(T x) {\n"         // line 24
        "    T copy = x;\n"                                // line 25
        "    return copy.MissingMemberOnLine26();\n"       // line 26
        "}\n";                                             // line 27

    const std::string appSource =
        "module Sprint2.DiagApp;\n"
        "import Sprint2.DiagLib;\n"
        "int main() {\n"
        "    var b = new GenericBox<Payload>(new Payload());\n"
        "    return b.BrokenMethod() + GenericFreeBroken<Payload>(new Payload());\n"
        "}\n";

    realscript::compiler::Compilation compilation({
        {"src/lib/generic_template.rscript", templateSource},
        {"src/app/caller.rscript", appSource},
    });
    const auto build = compilation.build();
    require(build.diagnostics.hasErrors(),
        "Expected semantic errors inside instantiated generic templates");

    realscript::text::SourceText origText(templateSource, "src/lib/generic_template.rscript");
    bool sawLine19Error = false;
    bool sawLine26Error = false;
    for (const auto& diag : build.diagnostics.items()) {
        require(diag.sourceName.find("$generated") == std::string::npos,
            "Generic diagnostic must never point to synthetic $generated path, got: " +
                diag.sourceName);
        if (diag.sourceName == "src/lib/generic_template.rscript") {
            const auto pos = origText.linePosition(diag.span.start);
            const std::size_t line1Based = pos.line + 1;
            if (line1Based == 19) {
                sawLine19Error = true;
            }
            if (line1Based == 26) {
                sawLine26Error = true;
            }
        }
    }
    require(sawLine19Error,
        "Expected generic class method error mapped accurately to line 19; actual diagnostics:\n" +
            diagnosticsText(build.diagnostics));
    require(sawLine26Error,
        "Expected generic free function error mapped accurately to line 26; actual diagnostics:\n" +
            diagnosticsText(build.diagnostics));
}

// 2. lang_3(2) + P1-3 + P2-3a: Cross-module generic symbol hygiene, cross-module builtin container mangling,
//    non-leaky internal visibility, qualified constraints, and strict public `new()` checks
void testCrossModuleGenericHygiene() {
    const std::string modXSource = R"RS(
module Sprint2.ModX;

public interface ITag {
    int TagCode();
}

public class Item : ITag {
    public int xVal;
    public Item(int v) { xVal = v; }
    public int TagCode() { return xVal + 100; }
}
)RS";

    const std::string modYSource = R"RS(
module Sprint2.ModY;

public interface ITag {
    int TagCode();
}

public class Item : ITag {
    public int yVal;
    public Item(int v) { yVal = v; }
    public int TagCode() { return yVal + 200; }
}
)RS";

    const std::string libSource = R"RS(
module Sprint2.HygieneLib;
import Sprint2.ModX;

internal class InternalSecret {
    internal int secretCode;
    internal InternalSecret() {
        secretCode = 77;
    }
}

internal class InternalOnlyGeneric<T> {
    public T val;
}

public class Helper {
    public int seed;
    public Helper() {
        seed = 100;
    }
    public int Tag() {
        return seed + 7;
    }
}

int ComputeBias(int v) {
    return v + 1000;
}

public class HygienicVault<T> {
    Helper libHelper;
    InternalSecret secret;
    public T item;

    public HygienicVault(T value) {
        libHelper = new Helper();
        secret = new InternalSecret();
        item = value;
    }

    public int LibSignature() {
        return ComputeBias(libHelper.Tag()) + secret.secretCode - 77;
    }

    public T GetItem() {
        return item;
    }
}

public class TagExtractor<T> where T : Sprint2.ModX::ITag {
    public static int Read(T v) {
        return v.TagCode();
    }
}

public class Factory<T> where T : new() {
    public static T Create() {
        return new T();
    }
}

public int HygienicScore<T>(T ignoredValue) {
    Helper localLibHelper = new Helper();
    return ComputeBias(localLibHelper.Tag());
}

public int LibSelfCheck() {
    var internalVault = new HygienicVault<Helper>(new Helper());
    return internalVault.LibSignature() + internalVault.GetItem().Tag();
}
)RS";

    const std::string appSource = R"RS(
module Sprint2.HygieneApp;
import Sprint2.HygieneLib;
import Sprint2.ModX;
import Sprint2.ModY;

internal class AppInternal {
    internal int code;
    public AppInternal() {
        code = 33;
    }
}

public class Helper {
    public int appPower;
    public Helper(int p) {
        appPower = p;
    }
    public int AppOnlyValue() {
        return appPower * 3;
    }
    public int Tag() {
        return -9999;
    }
}

int ComputeBias(int v) {
    return -77777;
}

public class Hero {
    public int level;
    public Hero() {
        level = 12;
    }
    public Hero(int lv) {
        level = lv;
    }
}

public int main() {
    var appHelper = new Helper(15);
    var vaultWithCollidingArg = new HygienicVault<Helper>(appHelper);
    int part1 = vaultWithCollidingArg.LibSignature() + vaultWithCollidingArg.GetItem().AppOnlyValue();

    var heroVault = new HygienicVault<Hero>(new Hero(9));
    int part2 = heroVault.LibSignature() + heroVault.GetItem().level;

    int part3 = HygienicScore<Helper>(appHelper) + LibSelfCheck();

    // App's own internal type passed to Lib generic must work via scoped specialization hygiene
    var appInternalVault = new HygienicVault<AppInternal>(Factory<AppInternal>.Create());
    if (appInternalVault.GetItem().code != 33) {
        return -2;
    }

    // Cross-module builtin containers List<ModX::Item> and List<ModY::Item> in the same module
    // MUST NOT collide on `List__Item`!
    var lx = new List<Sprint2.ModX::Item>();
    lx.Add(new Sprint2.ModX::Item(5));
    var ly = new List<Sprint2.ModY::Item>();
    ly.Add(new Sprint2.ModY::Item(8));
    if (lx.Get(0).xVal != 5 || ly.Get(0).yVal != 8) {
        return -3;
    }
    if (TagExtractor<Sprint2.ModX::Item>.Read(lx.Get(0)) != 105) {
        return -4;
    }
    Hero madeHero = Factory<Hero>.Create();
    if (madeHero.level != 12) {
        return -5;
    }

    if (ComputeBias(1) != -77777) {
        return -1;
    }

    return part1 + part2 + part3;
}
)RS";

    realscript::compiler::Compilation compilation({
        {"mod_x.rscript", modXSource},
        {"mod_y.rscript", modYSource},
        {"hygiene_lib.rscript", libSource},
        {"hygiene_app.rscript", appSource},
    });
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Cross-module hygienic generic compilation failed:\n" +
            diagnosticsText(build.diagnostics));

    auto interpreter = buildInterpreter(build);
    const auto interpRes = interpreter.invoke("Sprint2.HygieneApp::main");
    require(interpRes.succeeded,
        "Interpreter failed in hygiene test: " + interpRes.error.message);
    require(std::get<std::int64_t>(interpRes.value) == 4589,
        "Unexpected hygiene result in Interpreter: " +
            std::to_string(std::get<std::int64_t>(interpRes.value)));

    realscript::aot::GenerationOptions opts;
    opts.programName = "HygieneProg";
    opts.cppNamespace = "rs_hygiene_aot";
    auto aotLib = compileAndLoadAot(build.modules, opts);
    realscript::aot::Program aotProg(*aotLib->descriptor);
    const auto aotRes = aotProg.invoke("Sprint2.HygieneApp::main");
    require(aotRes.succeeded, "AOT execution failed in hygiene test: " + aotRes.error.message);
    require(std::get<std::int64_t>(aotRes.value) == 4589,
        "AOT hygiene result mismatch vs Interpreter");

    // Negative check A: Consumer module MUST NOT gain access to Lib's `internal class InternalSecret`
    // even after instantiating `HygienicVault<Hero>`!
    {
        realscript::compiler::Compilation leakComp({
            {"mod_x.rscript", modXSource},
            {"hygiene_lib.rscript", libSource},
            {"bad_leak.rscript",
             "module Sprint2.BadLeak;\n"
             "import Sprint2.HygieneLib;\n"
             "public class Dummy {}\n"
             "int main() {\n"
             "    var v = new HygienicVault<Dummy>(new Dummy());\n"
             "    var illegal = new Sprint2.HygieneLib::InternalSecret();\n"
             "    return 0;\n"
             "}\n"},
        });
        const auto leakBuild = leakComp.build();
        require(leakBuild.diagnostics.hasErrors() && hasDiagnosticCode(leakBuild.diagnostics, "RS2534"),
            "Expected RS2534 when consumer module tries to access Lib internal type:\n" +
                diagnosticsText(leakBuild.diagnostics));
    }

    // Negative check B: Cross-module `internal` generic template must be rejected with RS2534
    {
        realscript::compiler::Compilation internalTplComp({
            {"mod_x.rscript", modXSource},
            {"hygiene_lib.rscript", libSource},
            {"bad_tpl.rscript",
             "module Sprint2.BadTpl;\n"
             "import Sprint2.HygieneLib;\n"
             "int main() {\n"
             "    var x = new InternalOnlyGeneric<int>();\n"
             "    return 0;\n"
             "}\n"},
        });
        const auto tplBuild = internalTplComp.build();
        require(tplBuild.diagnostics.hasErrors() && hasDiagnosticCode(tplBuild.diagnostics, "RS2534"),
            "Expected RS2534 when instantiating cross-module internal generic template:\n" +
                diagnosticsText(tplBuild.diagnostics));
    }

    // Negative check C: Qualified constraint `where T : Sprint2.ModX::ITag` must reject `Sprint2.ModY::Item`
    {
        realscript::compiler::Compilation wrongTagComp({
            {"mod_x.rscript", modXSource},
            {"mod_y.rscript", modYSource},
            {"hygiene_lib.rscript", libSource},
            {"bad_constraint.rscript",
             "module Sprint2.BadConstraint;\n"
             "import Sprint2.HygieneLib;\n"
             "import Sprint2.ModY;\n"
             "int main() {\n"
             "    return TagExtractor<Sprint2.ModY::Item>.Read(new Sprint2.ModY::Item(1));\n"
             "}\n"},
        });
        const auto wrongTagBuild = wrongTagComp.build();
        require(wrongTagBuild.diagnostics.hasErrors() && hasDiagnosticCode(wrongTagBuild.diagnostics, "RS8530"),
            "Expected RS8530 when ModY::Item is passed to ModX::ITag constraint:\n" +
                diagnosticsText(wrongTagBuild.diagnostics));
    }

    // Negative check D: `where T : new()` must reject private and internal parameterless constructors
    {
        realscript::compiler::Compilation privateCtorComp({
            {"mod_x.rscript", modXSource},
            {"hygiene_lib.rscript", libSource},
            {"bad_new_private.rscript",
             "module Sprint2.BadNew;\n"
             "import Sprint2.HygieneLib;\n"
             "public class LockedCtor {\n"
             "    private LockedCtor() {}\n"
             "}\n"
             "int main() {\n"
             "    var x = Factory<LockedCtor>.Create();\n"
             "    return 0;\n"
             "}\n"},
        });
        const auto privBuild = privateCtorComp.build();
        require(privBuild.diagnostics.hasErrors() && hasDiagnosticCode(privBuild.diagnostics, "RS8530"),
            "Expected RS8530 for `where T : new()` with private default constructor:\n" +
                diagnosticsText(privBuild.diagnostics));

        realscript::compiler::Compilation internalCtorComp({
            {"mod_x.rscript", modXSource},
            {"hygiene_lib.rscript", libSource},
            {"bad_new_internal.rscript",
             "module Sprint2.BadNewInternal;\n"
             "import Sprint2.HygieneLib;\n"
             "public class PackageCtor {\n"
             "    internal PackageCtor() {}\n"
             "}\n"
             "int main() {\n"
             "    var x = Factory<PackageCtor>.Create();\n"
             "    return 0;\n"
             "}\n"},
        });
        const auto intBuild = internalCtorComp.build();
        require(intBuild.diagnostics.hasErrors() && hasDiagnosticCode(intBuild.diagnostics, "RS8530"),
            "Expected RS8530 for `where T : new()` with non-public internal default constructor:\n" +
                diagnosticsText(intBuild.diagnostics));
    }
}

// 3. lang_3(3) + P2-1: Reference-Type AOT Shared Generic Code Folding + Scalar Fast-Path Priority + No `__` False Stem Folding
void testAotReferenceTypeGenericCodeFolding() {
    const std::string source = R"RS(
module Sprint2.AotFold;

public class Entity {}
public class Hero : Entity { public int hp; public Hero(int v) { hp = v; } }
public class Enemy : Entity { public int atk; public Enemy(int v) { atk = v; } }
public class Npc : Entity { public int gold; public Npc(int v) { gold = v; } }
public class Boss : Entity { public int phase; public Boss(int v) { phase = v; } }
public class Mage : Entity { public int mana; public Mage(int v) { mana = v; } }
public class Archer : Entity { public int range; public Archer(int v) { range = v; } }

public class RefCell<T> {
    T current;
    T backup;

    public RefCell(T init) {
        current = init;
        backup = init;
    }

    public T Get() {
        return current;
    }

    public void Update(T nextVal) {
        backup = current;
        current = nextVal;
    }

    public T Restore() {
        T prev = current;
        current = backup;
        return prev;
    }

    public bool MatchesType(Entity candidate) {
        return candidate is T;
    }

    public string DescribeTag(string prefix) {
        try {
            return prefix + ":ok";
        } catch {
            return prefix + ":err";
        }
    }

    public T SafeSwapWithGuard(T nextVal) {
        try {
            T oldVal = current;
            current = nextVal;
            return oldVal;
        } finally {
            backup = nextVal;
        }
    }

    public T SelectTried(bool useBackup, T alt) {
        try {
            if (useBackup) return backup;
            return current;
        } catch {
            return alt;
        }
    }

    public void ResetGuarded(T v) {
        try {
            current = v;
        } finally {
            backup = v;
        }
    }

    public bool CheckCandidateGuarded(Entity candidate) {
        try {
            return candidate is T;
        } catch {
            return false;
        }
    }
}

public T SelectRef<T>(bool pickFirst, T a, T b) {
    if (pickFirst) return a;
    return b;
}

public T SelectRefGuarded<T>(bool pickFirst, T a, T b) {
    try {
        if (pickFirst) return a;
        return b;
    } catch {
        return b;
    }
}

public int main() {
    var ch = new RefCell<Hero>(new Hero(10));
    ch.Update(new Hero(25));
    var ce = new RefCell<Enemy>(new Enemy(7));
    ce.Update(new Enemy(19));
    var cn = new RefCell<Npc>(new Npc(3));
    var cb = new RefCell<Boss>(new Boss(99));
    var cm = new RefCell<Mage>(new Mage(11));
    var ca = new RefCell<Archer>(new Archer(13));

    Hero selected = SelectRef<Hero>(false, new Hero(1), new Hero(5));
    Hero guarded = SelectRefGuarded<Hero>(true, selected, ch.Get());
    Enemy gEnemy = SelectRefGuarded<Enemy>(false, new Enemy(1), ce.Get());
    Npc gNpc = SelectRefGuarded<Npc>(true, cn.Get(), new Npc(0));
    Boss gBoss = SelectRefGuarded<Boss>(true, cb.Get(), new Boss(0));
    Mage gMage = SelectRefGuarded<Mage>(true, cm.Get(), new Mage(0));
    Archer gArcher = SelectRefGuarded<Archer>(true, ca.Get(), new Archer(0));

    var ci = new RefCell<int>(100);
    ci.Update(120);

    int typeChecks = 0;
    if (ch.MatchesType(new Hero(1)) && !ch.MatchesType(new Enemy(1))) {
        typeChecks = typeChecks + 10;
    }
    if (ce.CheckCandidateGuarded(new Enemy(2)) && !ce.CheckCandidateGuarded(new Boss(2))) {
        typeChecks = typeChecks + 20;
    }

    Hero prevH = ch.Restore();
    return prevH.hp + ch.Get().hp + guarded.hp +
        gEnemy.atk + gNpc.gold + gBoss.phase + (gMage.mana - 11) + (gArcher.range - 13) +
        ci.Get() + typeChecks;
}
)RS";

    realscript::compiler::Compilation compilation({{"aot_fold.rscript", source}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "AOT fold test script failed to compile:\n" + diagnosticsText(build.diagnostics));

    auto interpreter = buildInterpreter(build);
    const auto interpRes = interpreter.invoke("Sprint2.AotFold::main");
    require(interpRes.succeeded, "Interpreter failed on AotFold: " + interpRes.error.message);
    const auto expected = std::get<std::int64_t>(interpRes.value);
    require(expected == 311, "Unexpected Interpreter result on AotFold: " + std::to_string(expected));

    std::vector<realscript::mir::Module> foldOnlyModules;
    for (const auto& m : build.modules) {
        if (m.name == "Sprint2.AotFold") foldOnlyModules.push_back(m);
    }
    require(foldOnlyModules.size() == 1, "Expected Sprint2.AotFold module");

    realscript::aot::CppGenerator generator;
    realscript::diagnostics::DiagnosticBag diagShared;
    realscript::aot::GenerationOptions sharedOpts;
    sharedOpts.programName = "AotFoldShared";
    sharedOpts.cppNamespace = "rs_fold_shared";
    sharedOpts.enableSharedReferenceGenerics = true;
    const auto sharedGen = generator.generate(foldOnlyModules, diagShared, sharedOpts);
    require(!diagShared.hasErrors(),
        "Shared AOT generation failed:\n" + diagnosticsText(diagShared));

    realscript::diagnostics::DiagnosticBag diagUnshared;
    realscript::aot::GenerationOptions unsharedOpts = sharedOpts;
    unsharedOpts.programName = "AotFoldUnshared";
    unsharedOpts.cppNamespace = "rs_fold_unshared";
    unsharedOpts.enableSharedReferenceGenerics = false;
    const auto unsharedGen = generator.generate(foldOnlyModules, diagUnshared, unsharedOpts);
    require(!diagUnshared.hasErrors(),
        "Unshared AOT generation failed:\n" + diagnosticsText(diagUnshared));

    require(sharedGen.sharedGenericBodyCount >= 6,
        "Expected >= 6 shared reference-type generic C++ bodies, got " +
            std::to_string(sharedGen.sharedGenericBodyCount));
    require(sharedGen.foldedGenericFunctionCount >= 36,
        "Expected >= 36 reference-type generic functions folded into thunks, got " +
            std::to_string(sharedGen.foldedGenericFunctionCount));
    require(sharedGen.source.find("shared_ref_generic_0") != std::string::npos,
        "Generated AOT C++ must contain shared_ref_generic_* implementation bodies");
    require(sharedGen.source.size() + 150000 < unsharedGen.source.size(),
        "Reference-type generic sharing must substantially shrink non-scalar generic C++ output (shared=" +
            std::to_string(sharedGen.source.size()) + ", unshared=" +
            std::to_string(unsharedGen.source.size()) + ")");

    // Pure non-scalar reference generic module check: >25% whole-module C++ source size reduction
    {
        const std::string pureGuardedSrc = R"RS(
module Sprint2.PureBoxedFold;
public class E0 {}
public class E1 : E0 {}
public class E2 : E0 {}
public class E3 : E0 {}
public class E4 : E0 {}
public class E5 : E0 {}
public class E6 : E0 {}
public class E7 : E0 {}
public class GuardedVault<T> {
    T a;
    T b;
    public GuardedVault(T x) { try { a = x; } finally { b = x; } }
    public T Swap(T nextVal) { try { T oldVal = a; a = nextVal; return oldVal; } finally { b = nextVal; } }
    public T Pick(bool first, T fallback) { try { if (first) return a; return b; } catch { return fallback; } }
    public string Tag(string p) { try { return p + ":v"; } catch { return p; } }
}
public int main() {
    var v1 = new GuardedVault<E1>(new E1());
    var v2 = new GuardedVault<E2>(new E2());
    var v3 = new GuardedVault<E3>(new E3());
    var v4 = new GuardedVault<E4>(new E4());
    var v5 = new GuardedVault<E5>(new E5());
    var v6 = new GuardedVault<E6>(new E6());
    var v7 = new GuardedVault<E7>(new E7());
    if (v1.Tag("x") == "" || v2.Swap(null) == null || v3.Pick(true, null) == null ||
        v4.Tag("y") == "" || v5.Tag("z") == "" || v6.Tag("w") == "" || v7.Tag("q") == "") return -1;
    return 1;
}
)RS";
        realscript::compiler::Compilation pgComp({{"pure_fold.rscript", pureGuardedSrc}});
        const auto pgBuild = pgComp.build();
        require(!pgBuild.diagnostics.hasErrors(), "pure_fold failed to compile");
        std::vector<realscript::mir::Module> pgOnly;
        for (const auto& m : pgBuild.modules) {
            if (m.name == "Sprint2.PureBoxedFold") pgOnly.push_back(m);
        }
        realscript::diagnostics::DiagnosticBag d1, d2;
        const auto pgShared = generator.generate(pgOnly, d1, sharedOpts);
        const auto pgUnshared = generator.generate(pgOnly, d2, unsharedOpts);
        require(!d1.hasErrors() && !d2.hasErrors(), "pure_fold AOT generation failed");
        require(pgShared.source.size() * 100 < pgUnshared.source.size() * 75,
            "Pure non-scalar reference generic sharing must shrink module C++ source by >25% (shared=" +
                std::to_string(pgShared.source.size()) + ", unshared=" +
                std::to_string(pgUnshared.source.size()) + ")");
    }

    // P2-1 check 1: `List<Hero>.Count()` and `RefCell<Hero>.Get()` must stay on zero-allocation `_scalar` fast path
    // and NEVER be demoted into `shared_ref_generic_*`!
    {
        const std::string listScalarSrc = R"RS(
module Sprint2.ListScalarFastPath;
public class Hero { public int hp; public Hero(int v) { hp = v; } }
public class Enemy { public int atk; public Enemy(int v) { atk = v; } }
public int main() {
    var lh = new List<Hero>();
    lh.Add(new Hero(3));
    var le = new List<Enemy>();
    le.Add(new Enemy(4));
    return lh.Count() + le.Count();
}
)RS";
        realscript::compiler::Compilation lsComp({{"list_scalar.rscript", listScalarSrc}});
        const auto lsBuild = lsComp.build();
        require(!lsBuild.diagnostics.hasErrors(), "list_scalar failed to compile");
        realscript::diagnostics::DiagnosticBag lsDiag;
        const auto lsGen = generator.generate(lsBuild.modules, lsDiag, sharedOpts);
        require(!lsDiag.hasErrors(), "list_scalar AOT generation failed");
        const realscript::mir::Module* lsMod = nullptr;
        for (const auto& m : lsBuild.modules) {
            if (m.name == "Sprint2.ListScalarFastPath") lsMod = &m;
        }
        require(lsMod != nullptr, "Failed to find Sprint2.ListScalarFastPath module");
        realscript::semantic::SymbolId countHeroId = 0;
        for (const auto& fn : lsMod->functions) {
            if (fn.name == "List__Hero.Count") countHeroId = fn.symbolId;
        }
        require(countHeroId != 0, "Failed to find List__Hero.Count in MIR");
        std::ostringstream hexStream;
        hexStream << std::hex << countHeroId;
        std::string hexStr = hexStream.str();
        while (hexStr.size() < 16) hexStr = "0" + hexStr;
        const std::string scalarSig = "fn_" + hexStr + "_scalar(";
        const std::string sharedSpecName = "fn_" + hexStr + "_sharedSpec";
        require(lsGen.source.find(scalarSig) != std::string::npos,
            "List<Hero>.Count() must emit zero-allocation _scalar fast path (" + scalarSig + ")");
        require(lsGen.source.find(sharedSpecName) == std::string::npos,
            "List<Hero>.Count() must NOT be folded into boxed shared_ref_generic!");
    }

    // P2-1 check 2: User-defined `CustomA.__Load` / `CustomA.__Save` / `Helper__One` must NEVER be falsely folded!
    {
        const std::string onlyDunderSource = R"RS(
module Sprint2.NoFalseFold;
public class Item { public int v; public Item(int x) { v = x; } }
public Item Helper__One(Item a) { try { return a; } catch { return null; } }
public Item Helper__Two(Item a) { try { return a; } catch { return null; } }
public class CustomA {
    public Item __Load(Item a) { try { return a; } catch { return null; } }
    public Item __Save(Item a) { try { return a; } catch { return null; } }
}
public int main() {
    var c = new CustomA();
    var it = new Item(7);
    return Helper__One(c.__Load(it)).v + Helper__Two(c.__Save(it)).v;
}
)RS";
        realscript::compiler::Compilation noFoldComp({{"no_fold.rscript", onlyDunderSource}});
        const auto noFoldBuild = noFoldComp.build();
        require(!noFoldBuild.diagnostics.hasErrors(), "no_fold failed to compile");
        realscript::diagnostics::DiagnosticBag nfDiag;
        const auto nfGen = generator.generate(noFoldBuild.modules, nfDiag, sharedOpts);
        require(!nfDiag.hasErrors(), "no_fold AOT generation failed");
        require(nfGen.foldedGenericFunctionCount == 0 && nfGen.sharedGenericBodyCount == 0,
            "User functions containing '__' must NEVER be falsely folded as generics; got folded=" +
                std::to_string(nfGen.foldedGenericFunctionCount));
    }

    auto aotLib = compileAndLoadAot(build.modules, sharedOpts);
    realscript::aot::Program aotProg(*aotLib->descriptor);
    const auto aotRes = aotProg.invoke("Sprint2.AotFold::main");
    require(aotRes.succeeded, "Folded AOT execution failed: " + aotRes.error.message);
    require(std::get<std::int64_t>(aotRes.value) == expected,
        "Folded AOT execution diverged from Interpreter!");
}

// 4. lang_4(1) + P1-2 + P2-3a: Deterministic `using`, readonly `using` enforcement, bare embedded statement rejection,
//    `sequence` coroutine `using` + `yield`, and token-precise Resource auto-import
void testDeterministicUsingAllControlFlowPaths() {
    const std::string source = R"RS(
module Sprint2.UsingTests;

public class TraceLog {
    public static string events = "";
    public static void Reset() {
        events = "";
    }
    public static void Push(string tag) {
        events = events + "[" + tag + "]";
    }
}

public class TestError { public string msg; public TestError(string m) { msg = m; } }

public class ScopedProbe : IDisposable {
    string name;
    public ScopedProbe(string n) {
        name = n;
        TraceLog.Push("open:" + name);
    }
    public void Dispose() {
        TraceLog.Push("dispose:" + name);
    }
}

public struct StructGuard : IDisposable {
    public int id;
    public bool disposed;

    public StructGuard(int v) {
        id = v;
        disposed = false;
        TraceLog.Push("sopen:" + v);
    }

    public void Dispose() {
        disposed = true;
        TraceLog.Push("sdispose:" + id);
    }
}

int RunNormalAndDeclLifo() {
    using var a = new ScopedProbe("A");
    using ScopedProbe b = new ScopedProbe("B");
    using (var c = new ScopedProbe("C")) {
        using (IDisposable poly = new ScopedProbe("Poly")) {
            using (ScopedProbe nullProbe = null) {
                TraceLog.Push("body");
            }
        }
    }
    TraceLog.Push("after_block");
    return 10;
}

int RunEarlyReturn(int x) {
    using var outer = new ScopedProbe("RetOuter");
    if (x > 0) {
        using (var inner = new ScopedProbe("RetInner")) {
            using var guard = new StructGuard(7);
            TraceLog.Push("before_return");
            return x + 5;
        }
    }
    return 0;
}

int RunLoopBreakAndContinue() {
    int sum = 0;
    int i = 0;
    while (i < 4) {
        i = i + 1;
        using var iterScope = new ScopedProbe("L" + i);
        if (i == 2) {
            using (var contScope = new ScopedProbe("Cont")) {
                continue;
            }
        }
        if (i == 3) {
            using (var brkScope = new ScopedProbe("Brk")) {
                break;
            }
        }
        sum = sum + i;
    }
    return sum;
}

void ThrowInsideUsing() {
    using var u1 = new ScopedProbe("Ex1");
    using (var u2 = new ScopedProbe("Ex2")) {
        TraceLog.Push("throwing");
        throw new TestError("boom");
    }
}

public string RunSuite() {
    TraceLog.Reset();
    int v1 = RunNormalAndDeclLifo();
    int v2 = RunEarlyReturn(20);
    int v3 = RunLoopBreakAndContinue();
    try {
        ThrowInsideUsing();
    } catch {
        TraceLog.Push("caught");
    }
    TraceLog.Push("totals:" + (v1 + v2 + v3));
    return TraceLog.events;
}
)RS";

    realscript::compiler::Compilation compilation({{"using_suite.rscript", source}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "`using` test suite failed to compile:\n" + diagnosticsText(build.diagnostics));

    const std::string expectedTrace =
        "[open:A][open:B][open:C][open:Poly][body]"
        "[dispose:Poly][dispose:C][after_block][dispose:B][dispose:A]"
        "[open:RetOuter][open:RetInner][sopen:7][before_return]"
        "[sdispose:7][dispose:RetInner][dispose:RetOuter]"
        "[open:L1][dispose:L1]"
        "[open:L2][open:Cont][dispose:Cont][dispose:L2]"
        "[open:L3][open:Brk][dispose:Brk][dispose:L3]"
        "[open:Ex1][open:Ex2][throwing][dispose:Ex2][dispose:Ex1][caught]"
        "[totals:36]";

    auto heap = std::make_shared<realscript::runtime::ManagedHeap>();
    auto interpreter = buildInterpreter(build, heap);
    const auto interpRes = interpreter.invoke("Sprint2.UsingTests::RunSuite");
    require(interpRes.succeeded, "Interpreter failed on using suite: " + interpRes.error.message);
    std::string actualInterp;
    if (const auto* s = std::get_if<std::string>(&interpRes.value)) {
        actualInterp = *s;
    } else if (const auto* ref = std::get_if<realscript::runtime::ObjectRef>(&interpRes.value)) {
        const auto sv = heap->stringView(*ref);
        require(sv.has_value(), "Expected string return from RunSuite");
        actualInterp = std::string(*sv);
    }
    require(actualInterp == expectedTrace,
        "Interpreter `using` trace mismatch!\nExpected: " + expectedTrace +
            "\nActual:   " + actualInterp);

    realscript::aot::GenerationOptions opts;
    opts.programName = "UsingSuiteAot";
    opts.cppNamespace = "rs_using_aot";
    auto aotLib = compileAndLoadAot(build.modules, opts);
    auto aotHeap = std::make_shared<realscript::runtime::ManagedHeap>();
    realscript::aot::Program aotProg(*aotLib->descriptor);
    aotProg.setHeap(aotHeap);
    const auto aotRes = aotProg.invoke("Sprint2.UsingTests::RunSuite");
    require(aotRes.succeeded, "AOT execution failed on using suite: " + aotRes.error.message);
    std::string actualAot;
    if (const auto* s = std::get_if<std::string>(&aotRes.value)) {
        actualAot = *s;
    } else if (const auto* ref = std::get_if<realscript::runtime::ObjectRef>(&aotRes.value)) {
        const auto sv = aotHeap->stringView(*ref);
        require(sv.has_value(), "Expected string return from AOT RunSuite");
        actualAot = std::string(*sv);
    }
    require(actualAot == expectedTrace,
        "AOT `using` trace mismatch!\nExpected: " + expectedTrace +
            "\nActual:   " + actualAot);

    // Negative check 1: non-disposable type in `using` -> RS2650
    {
        realscript::compiler::Compilation invalidUsing({{
            "bad_using.rscript",
            "module Sprint2.BadUsing;\n"
            "void f() {\n"
            "    using var x = 123;\n"
            "}\n"}});
        const auto badBuild = invalidUsing.build();
        require(badBuild.diagnostics.hasErrors() && hasDiagnosticCode(badBuild.diagnostics, "RS2650"),
            "Expected RS2650 diagnostic for non-disposable `using` variable");
    }

    // P1-2 Negative check 2: Reassigning or passing `using` variable by `ref`/`out` -> RS2651
    {
        realscript::compiler::Compilation reassignUsing({{
            "reassign_using.rscript",
            "module Sprint2.ReadonlyUsing;\n"
            "class Probe : IDisposable { public void Dispose() {} }\n"
            "void MutateRef(ref Probe p) { p = null; }\n"
            "void MutateOut(out Probe p) { p = null; }\n"
            "void f() {\n"
            "    using var a = new Probe();\n"
            "    a = new Probe();\n"
            "    using (var b = new Probe()) {\n"
            "        MutateRef(ref b);\n"
            "        MutateOut(out b);\n"
            "    }\n"
            "}\n"}});
        const auto roBuild = reassignUsing.build();
        require(roBuild.diagnostics.hasErrors() && hasDiagnosticCode(roBuild.diagnostics, "RS2651"),
            "Expected RS2651 diagnostic when reassigning or passing `using` variable by ref/out:\n" +
                diagnosticsText(roBuild.diagnostics));
    }

    // P1-2 Negative check 3: Bare embedded `using var` under `if`/`while` -> RS1126
    {
        realscript::compiler::Compilation bareEmbeddedUsing({{
            "bare_embedded_using.rscript",
            "module Sprint2.BareEmbeddedUsing;\n"
            "class Probe : IDisposable { public void Dispose() {} }\n"
            "void f(bool cond) {\n"
            "    if (cond) using var a = new Probe();\n"
            "    while (cond) using var b = new Probe();\n"
            "}\n"}});
        const auto embBuild = bareEmbeddedUsing.build();
        require(embBuild.diagnostics.hasErrors() && hasDiagnosticCode(embBuild.diagnostics, "RS1126"),
            "Expected RS1126 syntax error for bare embedded `using` declaration:\n" +
                diagnosticsText(embBuild.diagnostics));
    }

    // P1-2 / P2-3a Positive check 4: `sequence` coroutine containing `using` with `yield` and post-`using` locals
    {
        realscript::game::GameApi api;
        auto host = std::make_shared<realscript::game::GameplayHost>(30, 11, 17);
        require(realscript::game::installGameplayBindings(api, host),
            "Failed to install gameplay bindings for sequence+using test");
        realscript::game::GameScriptCompiler seqCompiler(api);
        const auto seqCompiled = seqCompiler.compile({{"seq_using.rscript", R"RS(
module Sprint2.SeqUsing;
import RealScript.Game;

public class SeqActor {
    public int duringTick0;
    public int duringTick1;
    public int afterUsingLive;
    public int finalScore;

    sequence LoadAndUse(long target) {
        ResourceServer.ResetForTesting();
        using (var h = ResourceServer.Load("seq://hero.tex")) {
            duringTick0 = h.UseCount();
            yield wait_ticks(1);
            duringTick1 = h.IsValid() ? h.UseCount() : -99;
        }
        int postLocal = 40;
        afterUsingLive = ResourceServer.LiveCount();
        yield wait_ticks(1);
        finalScore = duringTick0 * 10 + duringTick1 * 10 + postLocal + afterUsingLive;
    }

    public int Score() {
        return finalScore;
    }

    public int AfterLive() {
        return afterUsingLive;
    }
}
)RS"}});
        require(seqCompiled.succeeded(),
            "sequence + using compilation failed:\n" + diagnosticsText(seqCompiled.diagnostics));

        realscript::game::ScriptRuntime scripts(seqCompiled.program);
        realscript::game::SceneScriptRuntime scene(scripts);
        require(scene.attach(77, "Sprint2.SeqUsing::SeqActor"), "attach SeqActor failed");
        scene.start();
        const auto startRes = scene.invoke(77, "LoadAndUse", {realscript::runtime::LongValue{77}});
        require(startRes.succeeded, "LoadAndUse start failed: " + startRes.error.message);

        realscript::game::SceneGameplayDriver driver(scene, host);
        // Advance 1 tick: resumes inside `using`, exits `using` (disposing `h`), suspends at second `yield`
        require(driver.advanceTicks(1).size() == 1, "first sequence tick did not fire");
        const auto liveCheck = scene.invoke(77, "AfterLive");
        require(liveCheck.succeeded && std::get<std::int64_t>(liveCheck.value) == 0,
            "`using` inside sequence must stay alive across yield and dispose immediately upon block exit");

        // Advance 2nd tick: resumes after `using` and computes `finalScore = 10 + 10 + 40 + 0 = 60`
        require(driver.advanceTicks(1).size() == 1, "second sequence tick did not fire");
        const auto scoreRes = scene.invoke(77, "Score");
        require(scoreRes.succeeded && std::get<std::int64_t>(scoreRes.value) == 60,
            "Unexpected sequence + using finalScore, got: " +
                (scoreRes.succeeded ? std::to_string(std::get<std::int64_t>(scoreRes.value)) : scoreRes.error.message));
    }

    // P2-3a Check 5: Token-precise `sourceMentionsResourcePrimitives` must ignore comments & string literals
    {
        const std::string noResourceCode =
            "module Sprint2.CleanTokens;\n"
            "// Comment mentioning Resource, ResourceHandle, ResourceServer, IDisposable, using\n"
            "/* Block comment with Resource and using */\n"
            "public string GetLabel() {\n"
            "    return \"String literal mentioning Resource ResourceHandle ResourceServer IDisposable\";\n"
            "}\n";
        realscript::compiler::Compilation tokenComp({{"clean_tokens.rscript", noResourceCode}});
        const auto tokenBuild = tokenComp.build();
        require(!tokenBuild.diagnostics.hasErrors(), "Clean token compilation failed");
        for (const auto& mod : tokenBuild.modules) {
            require(mod.name != "RealScript.Resource",
                "RealScript.Resource must NOT be auto-injected when keywords only appear in comments/strings");
        }
    }
}

// 5. lang_4(2) + P0-4a + P0-4b + P1-7: Independent ResourceHandle leases on copy/assign/arg/return/AsUntyped,
//    zero-leak table slot reuse + GC reclamation, and atomic UAF-safe C++ ResourceRegistry
class HostMeshResource final : public realscript::game::HostResource {
public:
    HostMeshResource(std::string path, std::string guid, std::atomic<int>* destroyCounter)
        : HostResource(std::move(path), std::move(guid)),
          destroyCounter_(destroyCounter) {}

protected:
    void onDispose() override {
        if (destroyCounter_ != nullptr) {
            destroyCounter_->fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    std::atomic<int>* destroyCounter_ = nullptr;
};

void testUnifiedResourceModelAndHostBridge() {
    const std::string scriptModelSource = R"RS(
module Sprint2.ResourceModel;

public class TextureAsset : Resource {
    public static int destroyedCount = 0;
    public int width;

    public TextureAsset(string path, string guid, int w) : base(path, guid) {
        width = w;
    }

    public override void OnDispose() {
        destroyedCount = destroyedCount + 1;
    }
}

public class SpriteHolder {
    public ResourceHandle tex;
    public ResourceHandle<TextureAsset> typedTex;

    public void CaptureFromScopedLoad(string path) {
        using (var h1 = ResourceServer.Load(path)) {
            this.tex = h1;
            if (h1.UseCount() != 2) {
                throw new TextureAsset("bad", "", -1);
            }
        }
    }

    public void CaptureViaByValueParam(ResourceHandle incoming) {
        this.tex = incoming;
    }

    public static int InspectByValue(ResourceHandle h) {
        return h.IsValid() ? h.UseCount() : -1;
    }

    public static ResourceHandle LoadAndReturnPlain(string path) {
        var localH = ResourceServer.Load(path);
        return localH;
    }

    public static ResourceHandle LoadAndReturnFromUsing(string path) {
        using (var scopedH = ResourceServer.Load(path)) {
            return scopedH;
        }
    }
}

public int VerifyScriptResources() {
    ResourceServer.ResetForTesting();
    TextureAsset.destroyedCount = 0;

    // 1. P0-4a: `using (var h1 = ResourceServer.Load("hero.tex")) { this.tex = h1; }`
    // Exiting `using` disposes `h1`, while `holder.tex` MUST retain its own active lease (`UseCount == 1`)!
    {
        var holder = new SpriteHolder();
        holder.CaptureFromScopedLoad("hero.tex");
        if (!holder.tex.IsValid() || holder.tex.UseCount() != 1 || holder.tex.Get() == null) {
            return -101;
        }

        // Local copy initialization `var hCopy = holder.tex;` + by-value arg call
        {
            using var hCopy = holder.tex;
            if (holder.tex.UseCount() != 2 || !hCopy.IsValid()) return -102;
            // Passing lvalue `hCopy` by value clones for the callee parameter and auto-disposes on return
            int seenInCall = SpriteHolder.InspectByValue(hCopy);
            if (seenInCall != 3 || holder.tex.UseCount() != 2) return -103;
        }
        // `hCopy` disposed at scope exit -> `holder.tex` still valid with UseCount == 1!
        if (!holder.tex.IsValid() || holder.tex.UseCount() != 1 || holder.tex.Get() == null) {
            return -104;
        }

        // Overwriting `holder.tex` with a new resource must deterministically release the old lease ("hero.tex")!
        holder.CaptureViaByValueParam(SpriteHolder.LoadAndReturnPlain("shield.tex"));
        if (ResourceServer.LiveCount() != 1 || holder.tex.Path != "shield.tex" || holder.tex.UseCount() != 1) {
            return -105;
        }

        // Returning from inside `using` must clone before `using` disposes its scoped variable
        var fromUsing = SpriteHolder.LoadAndReturnFromUsing("sword.tex");
        if (!fromUsing.IsValid() || fromUsing.UseCount() != 1 || fromUsing.Get() == null) {
            return -106;
        }
        fromUsing.Dispose();
        holder.tex.Dispose();
        if (holder.tex.IsValid() || ResourceServer.LiveCount() != 0) {
            return -107;
        }
    }

    // 2. P0-4a: Typed `ResourceHandle<T>` copy, assignment, and `AsUntyped()` independent lease!
    {
        var tex = new TextureAsset("res://tex/hero.png", "guid-hero-tex", 512);
        ResourceHandle untypedEscaped = ResourceHandle.Empty();
        {
            using var hTyped = ResourceHandle<TextureAsset>.Adopt(tex);
            if (hTyped.UseCount() != 1 || hTyped.Get().width != 512) return -110;
            untypedEscaped = hTyped.AsUntyped();
            if (tex.UseCount() != 2 || !untypedEscaped.IsValid()) return -111;

            using var hTypedCopy = hTyped;
            if (tex.UseCount() != 3 || hTypedCopy.Get().width != 512) return -112;
            hTypedCopy.Release();
            if (tex.UseCount() != 2 || tex.IsDisposed()) return -113;
        }
        // `hTyped` disposed at scope exit, but `untypedEscaped` MUST still hold its own lease!
        if (TextureAsset.destroyedCount != 0 || !untypedEscaped.IsValid() ||
            untypedEscaped.UseCount() != 1 || untypedEscaped.Get() == null) {
            return -114;
        }
        untypedEscaped.Dispose();
        if (TextureAsset.destroyedCount != 1 || untypedEscaped.IsValid() || ResourceServer.LiveCount() != 0) {
            return -115;
        }
    }

    // 3. Path & GUID deduplication
    {
        using var r1 = ResourceHandle.LoadWithGuid("res://audio/bgm.ogg", "guid-bgm-01");
        if (r1.UseCount() != 1 || r1.Path != "res://audio/bgm.ogg" || r1.Guid != "guid-bgm-01") {
            return -120;
        }
        {
            using var r2 = ResourceHandle.Load("res://audio/bgm.ogg");
            using var r3 = ResourceHandle.LoadWithGuid("res://ignored.ogg", "guid-bgm-01");
            if (r1.UseCount() != 3 || ResourceServer.LiveCount() != 1) return -121;
        }
        if (r1.UseCount() != 1 || r1.IsDisposed()) return -122;
    }
    if (ResourceServer.LiveCount() != 0) return -123;
    return 100 + TextureAsset.destroyedCount;
}

public int ChurnResourcesForLeakCheck(int iterations) {
    ResourceServer.ResetForTesting();
    TextureAsset.destroyedCount = 0;
    int i = 0;
    while (i < iterations) {
        var asset = new TextureAsset("res://churn/item_" + i, "guid-churn-" + i, i);
        using (var h = ResourceHandle<TextureAsset>.Adopt(asset)) {
            using var u = h.AsUntyped();
            if (!u.IsValid() || u.UseCount() != 2) return -1;
        }
        // Also churn WrapHost to verify zero wasted script slots and __disposed reset!
        Resource wrapped = Resource.WrapHost(9000 + i, "host://p", "host-g");
        if (wrapped.IsDisposed()) return -2;
        using (var wh = wrapped.AdoptHandle()) {
            if (!wh.IsValid()) return -3;
        }
        if (!wrapped.IsDisposed()) return -4;
        i = i + 1;
    }
    if (ResourceServer.LiveCount() != 0) return -5;
    // Free-list reuse must bound high-water marks to O(1) instead of O(iterations)!
    if (ResourceServer.__DebugResourceHighWaterMark() > 4) return -6;
    if (ResourceServer.__DebugLeaseHighWaterMark() > 4) return -7;
    return TextureAsset.destroyedCount;
}
)RS";

    realscript::compiler::Compilation scriptComp({{"resource_model.rscript", scriptModelSource}});
    const auto scriptBuild = scriptComp.build();
    require(!scriptBuild.diagnostics.hasErrors(),
        "Script resource model compilation failed:\n" + diagnosticsText(scriptBuild.diagnostics));

    auto modelHeap = std::make_shared<realscript::runtime::ManagedHeap>();
    auto interp = buildInterpreter(scriptBuild, modelHeap);
    const auto resA = interp.invoke("Sprint2.ResourceModel::VerifyScriptResources");
    require(resA.succeeded && std::get<std::int64_t>(resA.value) == 101,
        "Script resource model verification failed in Interpreter, got: " +
            (resA.succeeded ? std::to_string(std::get<std::int64_t>(resA.value)) : resA.error.message));

    // P0-4b GC + High-Water-Mark leak verification: 150 churned resources must be reclaimed by GC!
    {
        const auto warmRes = interp.invoke(
            "Sprint2.ResourceModel::ChurnResourcesForLeakCheck",
            {std::int64_t{2}});
        require(warmRes.succeeded && std::get<std::int64_t>(warmRes.value) == 2,
            "Warmup churn failed");
        modelHeap->collect();
        const std::size_t baselineLive = modelHeap->liveObjects();

        const auto churnRes = interp.invoke(
            "Sprint2.ResourceModel::ChurnResourcesForLeakCheck",
            {std::int64_t{150}});
        require(churnRes.succeeded && std::get<std::int64_t>(churnRes.value) == 150,
            "ChurnResourcesForLeakCheck failed, got: " +
                (churnRes.succeeded ? std::to_string(std::get<std::int64_t>(churnRes.value)) : churnRes.error.message));
        modelHeap->collect();
        const std::size_t afterChurnLive = modelHeap->liveObjects();
        require(afterChurnLive <= baselineLive + 8,
            "ResourceServer leaked strong GC references across 150 disposals! baseline=" +
                std::to_string(baselineLive) + ", after=" + std::to_string(afterChurnLive));
    }

    // Also run VerifyScriptResources under AOT via ToolchainJit!
    {
        realscript::aot::GenerationOptions modelAotOpts;
        modelAotOpts.programName = "ResourceModelAot";
        modelAotOpts.cppNamespace = "rs_res_model_aot";
        auto aotLib = compileAndLoadAot(scriptBuild.modules, modelAotOpts);
        realscript::aot::Program aotProg(*aotLib->descriptor);
        const auto aotResA = aotProg.invoke("Sprint2.ResourceModel::VerifyScriptResources");
        require(aotResA.succeeded && std::get<std::int64_t>(aotResA.value) == 101,
            "AOT VerifyScriptResources failed, got: " +
                (aotResA.succeeded ? std::to_string(std::get<std::int64_t>(aotResA.value)) : aotResA.error.message));
    }

    // Part B: Unified Host C++ `realscript::game::ResourceRegistry` <-> Script `using` bridge (Interpreter + AOT)
    const std::string hostBridgeSource = R"RS(
module Sprint2.HostBridge;

public class BridgeHolder {
    public static ResourceHandle persisted;
}

public int UseHostMeshInScript() {
    using var h1 = ResourceHandle.Load("res://meshes/dragon.mesh");
    if (!h1.IsValid() || h1.Guid != "guid-dragon-99") return -1;
    int duringFirst = h1.UseCount();
    {
        using (var h2 = h1) {
            if (h2.UseCount() != duringFirst + 1) return -2;
        }
    }
    if (h1.UseCount() != duringFirst) return -3;
    return duringFirst;
}
)RS";

    realscript::compiler::Compilation bridgeComp({{"host_bridge.rscript", hostBridgeSource}});
    const auto bridgeBuild = bridgeComp.build();
    require(!bridgeBuild.diagnostics.hasErrors(),
        "Host bridge script compilation failed:\n" + diagnosticsText(bridgeBuild.diagnostics));

    // Verify with Interpreter + P1-7 entry erasure on zero-ref
    {
        std::atomic<int> hostDestroyed{0};
        realscript::game::ResourceRegistry registry;
        auto hostHandle = registry.emplace<HostMeshResource>(
            "res://meshes/dragon.mesh", "guid-dragon-99",
            "res://meshes/dragon.mesh", "guid-dragon-99", &hostDestroyed);
        require(hostHandle.isValid() && hostHandle.useCount() == 1, "Initial host handle useCount must be 1");
        require(registry.entryCount() == 1, "Registry should have 1 entry while live");

        auto heap = std::make_shared<realscript::runtime::ManagedHeap>();
        auto bindings = std::make_shared<realscript::runtime::BindingRegistry>();
        registry.bindTo(*bindings, heap.get());

        auto bridgeInterp = buildInterpreter(bridgeBuild, heap, bindings);
        require(realscript::game::ResourceRegistry::enableInInterpreter(bridgeInterp),
            "Failed to enable host resource bridge in Interpreter");

        const auto callRes = bridgeInterp.invoke("Sprint2.HostBridge::UseHostMeshInScript");
        require(callRes.succeeded && std::get<std::int64_t>(callRes.value) == 2,
            "Script `using` over host resource should see UseCount == 2 while held by C++ + Script");
        require(hostHandle.useCount() == 1 && !hostHandle.isDisposed() && hostDestroyed.load() == 0,
            "After script `using` exits, host handle must drop back to UseCount == 1");

        hostHandle.dispose();
        require(hostHandle.isDisposed() && hostDestroyed.load() == 1 &&
                registry.liveCount() == 0 && registry.entryCount() == 0,
            "Disposing final C++ ResourceHandle must erase registry entry and trigger zero-ref cleanup");
    }

    // Verify with AOT via ToolchainJit
    {
        std::atomic<int> hostDestroyed{0};
        realscript::game::ResourceRegistry registry;
        auto hostHandle = registry.emplace<HostMeshResource>(
            "res://meshes/dragon.mesh", "guid-dragon-99",
            "res://meshes/dragon.mesh", "guid-dragon-99", &hostDestroyed);

        realscript::aot::GenerationOptions opts;
        opts.programName = "HostBridgeAot";
        opts.cppNamespace = "rs_host_bridge_aot";
        auto aotLib = compileAndLoadAot(bridgeBuild.modules, opts);
        auto heap = std::make_shared<realscript::runtime::ManagedHeap>();
        auto bindings = std::make_shared<realscript::runtime::BindingRegistry>();
        registry.bindTo(*bindings, heap.get());

        realscript::aot::Program aotProg(*aotLib->descriptor);
        aotProg.setHeap(heap);
        aotProg.setBindings(bindings);
        require(realscript::game::ResourceRegistry::enableInAot(aotProg),
            "Failed to enable host resource bridge in AOT Program");

        const auto callRes = aotProg.invoke("Sprint2.HostBridge::UseHostMeshInScript");
        require(callRes.succeeded && std::get<std::int64_t>(callRes.value) == 2,
            "AOT script `using` over host resource should see UseCount == 2");
        require(hostHandle.useCount() == 1 && hostDestroyed.load() == 0,
            "After AOT script `using` exits, host handle must return to UseCount == 1");

        hostHandle.dispose();
        require(hostDestroyed.load() == 1 && registry.disposedCount() == 1 &&
                registry.liveCount() == 0 && registry.entryCount() == 0,
            "Final C++ ResourceHandle release after AOT execution must reclaim resource deterministically");
    }

    // P1-7 Check A: External `ResourceHandle<T>` outliving `ResourceRegistry` destruction (NO UAF!)
    // + `clear()` preserving monotonic `nextId_` + multithreaded atomic retain/release safety!
    {
        std::atomic<int> outlivedDestroyed{0};
        realscript::game::ResourceHandle<HostMeshResource> outlivingHandle;
        std::int64_t idBeforeClear = 0;
        std::int64_t idAfterClear = 0;
        {
            realscript::game::ResourceRegistry transientRegistry;
            auto first = transientRegistry.emplace<HostMeshResource>(
                "res://tmp/a.mesh", "guid-a", "res://tmp/a.mesh", "guid-a", &outlivedDestroyed);
            idBeforeClear = first.resourceId();
            transientRegistry.clear();
            require(transientRegistry.entryCount() == 0, "clear() must empty registry entries");

            outlivingHandle = transientRegistry.emplace<HostMeshResource>(
                "res://tmp/b.mesh", "guid-b", "res://tmp/b.mesh", "guid-b", &outlivedDestroyed);
            idAfterClear = outlivingHandle.resourceId();
            require(idAfterClear > idBeforeClear,
                "ResourceRegistry::clear() must never reset nextId_ back to 1 (before=" +
                    std::to_string(idBeforeClear) + ", after=" + std::to_string(idAfterClear) + ")");
            first.dispose();
        }
        // `transientRegistry` is now destroyed! Multithreaded copy + dispose on `outlivingHandle` must be UAF-free!
        require(outlivingHandle.isValid() && outlivedDestroyed.load() == 1,
            "First handle should be disposed while outlivingHandle remains valid");
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([h = outlivingHandle]() mutable {
                for (int k = 0; k < 50; ++k) {
                    auto copy = h;
                    require(copy.useCount() >= 2, "concurrent copy must retain atomically");
                }
                h.dispose();
            });
        }
        for (auto& w : workers) w.join();
        outlivingHandle.dispose();
        require(outlivingHandle.isDisposed() && outlivedDestroyed.load() == 2,
            "Outliving handle must dispose cleanly exactly once without UAF after Registry destruction");
    }
}

// 6. P1-3: Builtin collections clearing vacated slots for GC, clamping <=0 capacity, and null-safe default Enumerators
void testCollectionSlotClearingAndEdgeCases() {
    const std::string source = R"RS(
module Sprint2.CollectionFixes;

public class BigPayload {
    public int[] blob;
    public int tag;
    public BigPayload(int t) {
        blob = new int[32];
        blob[0] = t;
        tag = t;
    }
}

public class Holder {
    public static List<BigPayload> sList;
    public static Queue<BigPayload> sQueue;
    public static Stack<BigPayload> sStack;
    public static HashSet<BigPayload> sSet;
    public static Dictionary<int, BigPayload> sDict;

    public static List__int__Enumerator defaultListEnum;
    public static Queue__int__Enumerator defaultQueueEnum;
    public static Stack__int__Enumerator defaultStackEnum;
    public static HashSet__int__Enumerator defaultSetEnum;
    public static Dictionary__int__int__Enumerator defaultDictEnum;

    public static void InitContainers() {
        sList = new List<BigPayload>(0);
        sQueue = new Queue<BigPayload>(0);
        sStack = new Stack<BigPayload>(0);
        sSet = new HashSet<BigPayload>(0);
        sDict = new Dictionary<int, BigPayload>(0);
    }

    public static void Populate(int n) {
        int i = 0;
        while (i < n) {
            sList.Add(new BigPayload(i));
            sQueue.Enqueue(new BigPayload(i + 1000));
            sStack.Push(new BigPayload(i + 2000));
            sSet.Add(new BigPayload(i + 3000));
            sDict.Put(i, new BigPayload(i + 4000));
            i = i + 1;
        }
    }

    public static int RemoveAllIndividually(int n) {
        int removed = 0;
        int i = n - 1;
        while (i >= 0) {
            sList.RemoveAt(i);
            var q = sQueue.Dequeue();
            var st = sStack.Pop();
            if (sDict.Remove(i)) removed = removed + 1;
            if (q != null && st != null) removed = removed + 1;
            i = i - 1;
        }
        // Re-populate and test Clear() as well, plus HashSet.Remove!
        var single = new BigPayload(99999);
        sSet.Add(single);
        if (!sSet.Remove(single)) return -1;
        sSet.Clear();
        return removed;
    }

    public static void ClearAll() {
        sList.Clear();
        sQueue.Clear();
        sStack.Clear();
        sSet.Clear();
        sDict.Clear();
    }

    public static int TestZeroCapacityAndDefaultEnumerators() {
        var l = new List<int>(0);
        var q = new Queue<int>(0);
        var s = new Stack<int>(-5);
        var hs = new HashSet<int>(0);
        var d = new Dictionary<int, int>(-2);

        l.Add(11);
        q.Enqueue(22);
        s.Push(33);
        hs.Add(44);
        d.Put(1, 55);

        if (l.Get(0) + q.Dequeue() + s.Pop() != 66) return -10;
        if (!hs.Contains(44) || d.Get(1) != 55) return -11;

        // Default-constructed enumerators (__owner == null) must return false from MoveNext()!
        if (defaultListEnum.MoveNext()) return -20;
        if (defaultQueueEnum.MoveNext()) return -21;
        if (defaultStackEnum.MoveNext()) return -22;
        if (defaultSetEnum.MoveNext()) return -23;
        if (defaultDictEnum.MoveNext()) return -24;
        return 777;
    }
}
)RS";

    realscript::compiler::Compilation comp({{"collection_fixes.rscript", source}});
    const auto build = comp.build();
    require(!build.diagnostics.hasErrors(),
        "Collection fixes script failed to compile:\n" + diagnosticsText(build.diagnostics));

    auto heap = std::make_shared<realscript::runtime::ManagedHeap>();
    auto interp = buildInterpreter(build, heap);

    const auto edgeRes = interp.invoke("Sprint2.CollectionFixes::Holder.TestZeroCapacityAndDefaultEnumerators");
    require(edgeRes.succeeded && std::get<std::int64_t>(edgeRes.value) == 777,
        "Zero-capacity / default Enumerator check failed: " +
            (edgeRes.succeeded ? std::to_string(std::get<std::int64_t>(edgeRes.value)) : edgeRes.error.message));

    require(interp.invoke("Sprint2.CollectionFixes::Holder.InitContainers").succeeded, "InitContainers failed");
    heap->collect();
    const std::size_t baselineLive = heap->liveObjects();

    require(interp.invoke("Sprint2.CollectionFixes::Holder.Populate", {std::int64_t{40}}).succeeded,
        "Populate failed");
    heap->collect();
    const std::size_t populatedLive = heap->liveObjects();
    require(populatedLive >= baselineLive + 200,
        "Expected >= 200 live BigPayload/array objects while held in containers");

    const auto remRes = interp.invoke("Sprint2.CollectionFixes::Holder.RemoveAllIndividually", {std::int64_t{40}});
    require(remRes.succeeded && std::get<std::int64_t>(remRes.value) == 80,
        "RemoveAllIndividually failed");
    heap->collect();
    const std::size_t afterIndividualRemove = heap->liveObjects();
    require(afterIndividualRemove <= baselineLive + 20,
        "Vacated collection slots retained stale GC references after RemoveAt/Dequeue/Pop/Remove! baseline=" +
            std::to_string(baselineLive) + ", after=" + std::to_string(afterIndividualRemove));

    // Populate again and verify Clear() releases all references too!
    require(interp.invoke("Sprint2.CollectionFixes::Holder.Populate", {std::int64_t{40}}).succeeded,
        "Second Populate failed");
    require(interp.invoke("Sprint2.CollectionFixes::Holder.ClearAll").succeeded,
        "ClearAll failed");
    heap->collect();
    const std::size_t afterClearLive = heap->liveObjects();
    require(afterClearLive <= baselineLive + 20,
        "Collection Clear() retained stale GC references! baseline=" +
            std::to_string(baselineLive) + ", after=" + std::to_string(afterClearLive));
}

} // namespace

int main() {
    try {
        std::cout << "[1/6] testGenericDiagnosticExactLineNumbers...\n" << std::flush;
        testGenericDiagnosticExactLineNumbers();
        std::cout << "[2/6] testCrossModuleGenericHygiene...\n" << std::flush;
        testCrossModuleGenericHygiene();
        std::cout << "[3/6] testAotReferenceTypeGenericCodeFolding...\n" << std::flush;
        testAotReferenceTypeGenericCodeFolding();
        std::cout << "[4/6] testDeterministicUsingAllControlFlowPaths...\n" << std::flush;
        testDeterministicUsingAllControlFlowPaths();
        std::cout << "[5/6] testUnifiedResourceModelAndHostBridge...\n" << std::flush;
        testUnifiedResourceModelAndHostBridge();
        std::cout << "[6/6] testCollectionSlotClearingAndEdgeCases...\n" << std::flush;
        testCollectionSlotClearingAndEdgeCases();
        std::cout << "All Sprint 2 generics, using, and Resource conformance tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
