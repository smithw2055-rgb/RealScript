#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/jit/Jit.h"
#include "realscript/mir/Mir.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string formatDiagnostics(const realscript::diagnostics::DiagnosticBag& bag) {
    std::ostringstream out;
    for (const auto& d : bag.items()) {
        out << "[" << d.code << "] " << d.message << "\n";
    }
    return out.str();
}

bool hasCode(
    const realscript::diagnostics::DiagnosticBag& bag,
    const std::string& code,
    realscript::diagnostics::DiagnosticSeverity severity) {
    for (const auto& d : bag.items()) {
        if (d.code == code && d.severity == severity) {
            return true;
        }
    }
    return false;
}

bool hasAnyCode(const realscript::diagnostics::DiagnosticBag& bag, const std::string& code) {
    for (const auto& d : bag.items()) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

struct BuiltBundle {
    realscript::diagnostics::DiagnosticBag diagnostics;
    std::vector<realscript::mir::Module> mirModules;
    std::shared_ptr<const realscript::runtime::ProgramImage> program;
};

BuiltBundle buildSources(std::vector<realscript::compiler::SourceFile> inputs) {
    BuiltBundle out;
    realscript::compiler::Compilation compilation(std::move(inputs));
    auto build = compilation.build();
    out.diagnostics = build.diagnostics;
    if (out.diagnostics.hasErrors()) {
        return out;
    }

    realscript::optimization::Options optOpts;
    optOpts.level = realscript::optimization::Level::Aggressive;
    auto optimized = realscript::optimization::Optimizer{}.optimize(
        std::move(build.modules), out.diagnostics, optOpts);
    if (out.diagnostics.hasErrors()) {
        return out;
    }
    out.mirModules = std::move(optimized.modules);

    realscript::bytecode::Lowerer bcLowerer;
    std::vector<realscript::bytecode::Module> bcModules;
    for (const auto& mirMod : out.mirModules) {
        auto bc = bcLowerer.lower(mirMod);
        (void)realscript::bytecode::verifyModule(bc, out.diagnostics);
        bcModules.push_back(std::move(bc));
    }
    if (out.diagnostics.hasErrors()) {
        return out;
    }

    realscript::runtime::RuntimeError linkErr;
    auto linked = realscript::runtime::ProgramImage::link(std::move(bcModules), linkErr);
    require(linked.has_value(), "ProgramImage::link failed: " + linkErr.message);
    out.program = std::make_shared<const realscript::runtime::ProgramImage>(std::move(*linked));
    return out;
}

void verifyThreeBackendIntResult(
    const BuiltBundle& bundle,
    const std::string& entryFn,
    std::int64_t expected) {
    require(bundle.program != nullptr, "bundle has null program");

    // 1. Bytecode Interpreter
    realscript::runtime::Interpreter interp(bundle.program);
    const auto interpRes = interp.invoke(entryFn);
    require(interpRes.succeeded, "Interpreter failed on " + entryFn + ": " + interpRes.error.message);
    require(
        std::holds_alternative<std::int64_t>(interpRes.value) &&
            std::get<std::int64_t>(interpRes.value) == expected,
        "Interpreter mismatch on " + entryFn + ": expected " + std::to_string(expected));

    // 2. In-Memory StencilJit
    realscript::jit::StencilJit sjit;
    auto compiledStencil = sjit.compile(bundle.program);
    require(compiledStencil.succeeded(), "StencilJit::compile failed: " + compiledStencil.error);
    const auto stencilRes = compiledStencil.module->invoke(entryFn);
    require(stencilRes.succeeded, "StencilJit failed on " + entryFn + ": " + stencilRes.error.message);
    require(
        std::holds_alternative<std::int64_t>(stencilRes.value) &&
            std::get<std::int64_t>(stencilRes.value) == expected,
        "StencilJit mismatch on " + entryFn);

    // 3. C++17 AOT Generator (+ Native ToolchainJit execution when configured)
    realscript::diagnostics::DiagnosticBag aotDiag;
    realscript::aot::CppGenerator cppGen;
    realscript::aot::GenerationOptions genOpts;
    genOpts.programName = "P3Stage3Verify";
    const auto generated = cppGen.generate(bundle.mirModules, aotDiag, genOpts);
    require(!aotDiag.hasErrors(), "AotCpp generate failed:\n" + formatDiagnostics(aotDiag));
    require(!generated.source.empty() && !generated.header.empty(), "AotCpp produced empty C++ output");

#if defined(REALSCRIPT_JIT_COMPILER) && defined(REALSCRIPT_JIT_INCLUDE_DIR) && defined(REALSCRIPT_JIT_SUPPORT_LIBRARY)
    realscript::jit::ToolchainOptions tcOpts;
    tcOpts.compiler = REALSCRIPT_JIT_COMPILER;
    tcOpts.includeDirectory = REALSCRIPT_JIT_INCLUDE_DIR;
    tcOpts.supportLibrary = REALSCRIPT_JIT_SUPPORT_LIBRARY;
#if defined(REALSCRIPT_JIT_CACHE_DIR)
    tcOpts.outputDirectory = REALSCRIPT_JIT_CACHE_DIR;
#else
    tcOpts.outputDirectory = std::filesystem::temp_directory_path() / "realscript_p3_s3_aot_cache";
#endif
#if defined(_WIN32)
#if defined(_DEBUG)
    tcOpts.compilerArguments = {"/MDd"};
#else
    tcOpts.compilerArguments = {"/MD", "/O2"};
#endif
#else
    tcOpts.compilerArguments = {"-O2"};
#endif
    tcOpts.reuseCachedLibrary = false;
    tcOpts.generation.programName = "P3Stage3AotExec";
    if (realscript::jit::ToolchainJit::available(tcOpts)) {
        realscript::jit::ToolchainJit tjit;
        const auto tcCompiled = tjit.compile(bundle.mirModules, tcOpts);
        require(tcCompiled.succeeded(), "C++17 AOT ToolchainJit failed: " + tcCompiled.error);
        const auto aotRes = tcCompiled.module->invoke(entryFn);
        require(aotRes.succeeded, "C++17 AOT invoke failed on " + entryFn + ": " + aotRes.error.message);
        require(
            std::holds_alternative<std::int64_t>(aotRes.value) &&
                std::get<std::int64_t>(aotRes.value) == expected,
            "C++17 AOT mismatch on " + entryFn);
    }
    std::error_code ec;
    std::filesystem::remove_all(tcOpts.outputDirectory, ec);
#endif
}

void testRichPatternMatchingFamilies() {
    const std::string src = R"RS(
module P3.Patterns;

public enum ThreatTier {
    Low = 1,
    Medium = 5,
    High = 10,
    Critical = 20
}

public struct Coords {
    public int X;
    public int Y;
}

public class Unit {
    public int Hp { get; set; } = 100;
    public bool IsBoss = false;
    public Coords Pos;
    public string Tag = "";

    public Unit(int hp, bool boss, int x, int y, string tag) {
        Hp = hp;
        IsBoss = boss;
        Pos = new Coords { X = x, Y = y };
        Tag = tag;
    }
}

public class EvalTracker {
    public static int Calls = 0;
    public static int NextValue(int v) {
        Calls = Calls + 1;
        return v;
    }
}

public int GradeScore(int score) {
    return score switch {
        < 0 or > 100 => -1,
        >= 90 => 4,
        >= 75 and < 90 => 3,
        >= 60 and < 75 => 2,
        _ => 1
    };
}

public int ClassifyChar(char c) {
    if (c is >= 'a' and <= 'z') return 1;
    if (c is >= 'A' and <= 'Z') return 2;
    if (c is >= '0' and <= '9') return 3;
    return 0;
}

public int ClassifyNullableAndEnum(int? maybeHp, ThreatTier tier) {
    int acc = 0;
    if (maybeHp is >= 50 and <= 150) {
        acc = acc + 10;
    }
    if (maybeHp is null) {
        acc = acc + 100;
    }
    if (tier is >= ThreatTier.High) {
        acc = acc + 5;
    }
    return acc;
}

public int InspectUnit(Unit u) {
    // Verify contextual keywords `and`, `or`, `not` still work as normal identifiers!
    int and = 1;
    int or = 2;
    int not = 3;
    int bonus = and + or + not; // 6

    switch (u) {
    case null:
        return -10 + bonus;
    case Unit { IsBoss: true, Hp: <= 0 } deadBoss:
        return 900 + deadBoss.Pos.X + bonus;
    case Unit { Pos: { X: >= 10, Y: >= 20 }, Tag: { Length: > 3 } } ranged:
        return 500 + ranged.Hp + bonus;
    case { Hp: > 0 and <= 25 }:
        return 200 + bonus;
    default:
        if (u is not null and { Hp: > 25 }) {
            return 100 + bonus;
        }
        return bonus;
    }
}

public int RunAllPatternChecks() {
    EvalTracker.Calls = 0;
    bool hit = EvalTracker.NextValue(42) is (>= 10 and <= 50) or 99;
    if (!hit || EvalTracker.Calls != 1) {
        return -999;
    }

    int g = GradeScore(-5) + GradeScore(95) * 10 + GradeScore(80) * 100 + GradeScore(65) * 1000 + GradeScore(20) * 10000;
    // -1 + 40 + 300 + 2000 + 10000 = 12339
    int ch = ClassifyChar('m') + ClassifyChar('Z') * 10 + ClassifyChar('7') * 100 + ClassifyChar('!') * 1000;
    // 1 + 20 + 300 + 0 = 321

    int? some = 80;
    int? none = null;
    int ne = ClassifyNullableAndEnum(some, ThreatTier.Critical) + ClassifyNullableAndEnum(none, ThreatTier.Low);
    // (10 + 5) + 100 = 115

    Unit uNull = null;
    Unit uDeadBoss = new Unit(0, true, 7, 0, "boss");
    Unit uRanged = new Unit(40, false, 12, 25, "sniper");
    Unit uLow = new Unit(15, false, 0, 0, "imp");
    Unit uHealthy = new Unit(80, false, 1, 1, "ok");

    int uSum = InspectUnit(uNull)       // -4
             + InspectUnit(uDeadBoss)   // 913
             + InspectUnit(uRanged)     // 546
             + InspectUnit(uLow)        // 206
             + InspectUnit(uHealthy);   // 106 -> total 1767

    return g + ch + ne + uSum; // 12339 + 321 + 115 + 1767 = 14542
}
)RS";

    const auto built = buildSources({{"patterns.rscript", src}});
    require(!built.diagnostics.hasErrors(), "Pattern suite failed to build:\n" + formatDiagnostics(built.diagnostics));
    verifyThreeBackendIntResult(built, "P3.Patterns::RunAllPatternChecks", 14542);

    // Negative test 1: pattern variable inside `or` / `not` -> RS8935
    {
        realscript::compiler::Compilation badOr({
            {"bad_or.rscript",
             "module P3.BadOr;\n"
             "public class Base {}\n"
             "public class A : Base {}\n"
             "public class B : Base {}\n"
             "bool Test(Base o) { return o is A a or B; }\n"}
        });
        const auto res = badOr.build();
        require(res.diagnostics.hasErrors() && hasAnyCode(res.diagnostics, "RS8935"),
            "Expected RS8935 for pattern variable inside disjunctive pattern:\n" + formatDiagnostics(res.diagnostics));
    }

    // Negative test 2: non-constant relational operand -> RS8936
    {
        realscript::compiler::Compilation badRel({
            {"bad_rel.rscript",
             "module P3.BadRel;\n"
             "bool Test(int x, int dynamicBound) { return x is > dynamicBound; }\n"}
        });
        const auto res = badRel.build();
        require(res.diagnostics.hasErrors() && hasAnyCode(res.diagnostics, "RS8936"),
            "Expected RS8936 for non-constant relational pattern operand:\n" + formatDiagnostics(res.diagnostics));
    }
}

void testDefaultInterfaceMethodsAndGenericExtensions() {
    const std::string traitLib = R"RS(
module P3.TraitLib;

public interface IDamageable {
    int BaseArmor();

    int Mitigate(int rawDamage) {
        int reduced = rawDamage - BaseArmor();
        return reduced > 0 ? reduced : 0;
    }

    bool IsLethalHit(int currentHp, int rawDamage) {
        return Mitigate(rawDamage) >= currentHp;
    }
}

public interface IScalable<T> {
    T ScaleBy(T factor);
    int DefaultTier() {
        return 3;
    }
}

public struct ShieldBlock : IDamageable, IScalable<int> {
    public int Armor;
    public int BaseArmor() { return Armor; }
    public int ScaleBy(int factor) { return Armor * factor; }
}

public class Turret : IDamageable {
    int armor;
    public Turret(int a) { armor = a; }
    public int BaseArmor() { return armor; }
}

public class BossTurret : Turret {
    public BossTurret(int a) : base(a) {}
    public override int Mitigate(int rawDamage) {
        int half = rawDamage / 2;
        int reduced = half - BaseArmor();
        return reduced > 0 ? reduced : 0;
    }
}

public static class MathExt {
    public static T ClampLow<T>(this T value, T minVal) {
        return value;
    }
}

public class Bag<T> {
    T[] storage;
    int count;
    public Bag() {
        storage = new T[8];
        count = 0;
    }
    public void Push(T item) {
        storage[count] = item;
        count = count + 1;
    }
    public T At(int idx) { return storage[idx]; }
    public int Size() { return count; }
}

public int CountAbove(this int[] items, int threshold) {
    int c = 0;
    foreach (int v in items) {
        if (v > threshold) {
            c = c + 1;
        }
    }
    return c;
}

public FirstValue<T> WrapFirst<T>(this Bag<T> items) {
    return new FirstValue<T>(items.At(0), items.Size());
}

public struct FirstValue<T> {
    public T Item;
    public int Total;
    public FirstValue(T item, int total) {
        Item = item;
        Total = total;
    }
}

public void OffsetShield(ref this ShieldBlock s, int delta) {
    s.Armor = s.Armor + delta;
}
)RS";

    const std::string appSrc = R"RS(
module P3.TraitApp;
import P3.TraitLib;

public int EvalViaInterface(IDamageable d, int hp, int dmg) {
    int m = d.Mitigate(dmg);
    return d.IsLethalHit(hp, dmg) ? (m + 1000) : m;
}

public int RunTraitAndExtSuite() {
    Turret normal = new Turret(10);
    BossTurret boss = new BossTurret(10);

    // Concrete + polymorphic interface calls on default methods
    int a = EvalViaInterface(normal, 25, 30); // Mitigate=20 < 25 -> 20
    int b = EvalViaInterface(normal, 20, 30); // Mitigate=20 >= 20 -> 1020
    int c = EvalViaInterface(boss, 20, 40);   // Boss override: 40/2 - 10 = 10 < 20 -> 10

    // Struct implementing default interface methods + mutating `ref this` extension method
    ShieldBlock sb = new ShieldBlock { Armor = 6 };
    sb.OffsetShield(4); // Armor becomes 10
    int sm = sb.Mitigate(18) + sb.DefaultTier() + sb.ScaleBy(2); // 8 + 3 + 20 = 31

    // Cross-module generic & array/container extension methods
    int[] arr = new int[3];
    arr[0] = 5;
    arr[1] = 15;
    arr[2] = 25;
    int above = arr.CountAbove(10); // 2

    var bag = new Bag<int>();
    bag.Push(5);
    bag.Push(15);
    bag.Push(25);
    var wrapped = bag.WrapFirst();   // Item=5, Total=3
    int clamped = (42).ClampLow(10); // 42

    return a + b + c + sm + above * 100 + wrapped.Item + wrapped.Total + clamped;
    // 20 + 1020 + 10 + 31 + 200 + 5 + 3 + 42 = 1331
}
)RS";

    const auto built = buildSources({{"trait_lib.rscript", traitLib}, {"trait_app.rscript", appSrc}});
    require(!built.diagnostics.hasErrors(), "Trait/Extension suite failed to build:\n" + formatDiagnostics(built.diagnostics));
    verifyThreeBackendIntResult(built, "P3.TraitApp::RunTraitAndExtSuite", 1331);

    // Diamond default interface conflict -> RS3415 unless explicitly overridden
    {
        realscript::compiler::Compilation diamondBad({
            {"diamond_bad.rscript",
             "module P3.DiamondBad;\n"
             "public interface IA { int Calc() { return 1; } }\n"
             "public interface IB { int Calc() { return 2; } }\n"
             "public class Conflict : IA, IB {}\n"}
        });
        const auto res = diamondBad.build();
        require(res.diagnostics.hasErrors() && hasAnyCode(res.diagnostics, "RS3415"),
            "Expected RS3415 for ambiguous diamond default interface method:\n" + formatDiagnostics(res.diagnostics));
    }

    // Rvalue receiver passed to `ref this` mutating extension method -> compile error
    {
        realscript::compiler::Compilation badRval({
            {"trait_lib.rscript", traitLib},
            {"bad_rval.rscript",
             "module P3.BadRval;\n"
             "import P3.TraitLib;\n"
             "ShieldBlock MakeShield() { return new ShieldBlock { Armor = 3 }; }\n"
             "void Test() { MakeShield().OffsetShield(5); }\n"}
        });
        const auto res = badRval.build();
        require(res.diagnostics.hasErrors(),
            "Expected compile error when calling `ref this` extension method on rvalue receiver");
    }
}

void testStrongAttributeContracts() {
    // 1. [Obsolete] warning (RS4101) without error
    {
        const std::string warnSrc = R"RS(
module P3.AttrWarn;

[AttributeUsage("class|method", allowMultiple: false)]
public class EndpointAttribute {
    public string Route;
    public EndpointAttribute(string route) { Route = route; }
}

[AttributeUsage("method", allowMultiple: true)]
public class TagAttribute {
    public string Name;
    public TagAttribute(string name) { Name = name; }
}

[Endpoint("/combat")]
public class CombatService {
    [Obsolete("use ComputeDamageV2 instead")]
    public static int ComputeDamageV1(int x) { return x + 1; }

    [Endpoint("/v2")]
    [Tag("fast")]
    [Tag("deterministic")]
    public static int ComputeDamageV2(int x) { return x * 2; }
}

public int CallBoth() {
    return CombatService.ComputeDamageV1(10) + CombatService.ComputeDamageV2(10);
}
)RS";
        const auto built = buildSources({{"attr_warn.rscript", warnSrc}});
        require(!built.diagnostics.hasErrors(), "AttrWarn should succeed with warning:\n" + formatDiagnostics(built.diagnostics));
        require(
            hasCode(built.diagnostics, "RS4101", realscript::diagnostics::DiagnosticSeverity::Warning),
            "Expected RS4101 warning on [Obsolete] method call:\n" + formatDiagnostics(built.diagnostics));
        verifyThreeBackendIntResult(built, "P3.AttrWarn::CallBoth", 31);
    }

    // 2. [Obsolete("...", true)] hard error -> RS4102
    {
        realscript::compiler::Compilation obsErr({
            {"obs_err.rscript",
             "module P3.ObsErr;\n"
             "public class Legacy {\n"
             "    [Obsolete(\"removed API\", true)]\n"
             "    public static int DeadCall() { return 0; }\n"
             "}\n"
             "public int Main() { return Legacy.DeadCall(); }\n"}
        });
        const auto res = obsErr.build();
        require(res.diagnostics.hasErrors() && hasAnyCode(res.diagnostics, "RS4102"),
            "Expected RS4102 error for [Obsolete(..., true)]:\n" + formatDiagnostics(res.diagnostics));
    }

    // 3. [AttributeUsage] invalid target -> RS4103
    {
        realscript::compiler::Compilation badTarget({
            {"bad_target.rscript",
             "module P3.BadAttrTarget;\n"
             "[AttributeUsage(\"method\", allowMultiple: false)]\n"
             "public class MethodOnlyAttribute {}\n"
             "public class Sample {\n"
             "    [MethodOnly]\n"
             "    public int BadField;\n"
             "}\n"}
        });
        const auto res = badTarget.build();
        require(res.diagnostics.hasErrors() && hasAnyCode(res.diagnostics, "RS4103"),
            "Expected RS4103 for attribute applied to disallowed target:\n" + formatDiagnostics(res.diagnostics));
    }

    // 4. [AttributeUsage(..., allowMultiple: false)] duplicate application -> RS4104
    {
        realscript::compiler::Compilation badDup({
            {"bad_dup.rscript",
             "module P3.BadAttrDup;\n"
             "[AttributeUsage(\"class\", allowMultiple: false)]\n"
             "public class SingletonTagAttribute {}\n"
             "[SingletonTag]\n"
             "[SingletonTagAttribute]\n"
             "public class DuplicateMarked {}\n"}
        });
        const auto res = badDup.build();
        require(res.diagnostics.hasErrors() && hasAnyCode(res.diagnostics, "RS4104"),
            "Expected RS4104 for duplicate single-use attribute:\n" + formatDiagnostics(res.diagnostics));
    }
}

} // namespace

int main() {
    try {
        std::cout << "[1/3] Testing rich pattern matching (relational, and/or/not, property patterns & single-eval) across Interpreter/StencilJit/AOT...\n";
        testRichPatternMatchingFamilies();
        std::cout << "[2/3] Testing default interface Trait methods, diamond RS3415, generic & `ref this` extension methods across Interpreter/StencilJit/AOT...\n";
        testDefaultInterfaceMethodsAndGenericExtensions();
        std::cout << "[3/3] Testing [Obsolete] (RS4101/RS4102) and [AttributeUsage] (RS4103/RS4104) strong contracts...\n";
        testStrongAttributeContracts();
        std::cout << "All P3 Stage 3 language extensibility & 3-backend parity tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << "\n";
        return 1;
    }
}
