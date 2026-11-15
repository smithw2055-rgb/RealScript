#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/compiler/ProjectConfig.h"
#include "realscript/compiler/StandardMacros.h"
#include "realscript/debug/DebugInfo.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/semantic/Semantic.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string diagnosticsText(
    const diagnostics::DiagnosticBag& bag,
    const text::SourceText* source = nullptr) {
    std::ostringstream out;
    for (const auto& d : bag.items()) {
        if (source) {
            out << diagnostics::formatDiagnostic(d, *source) << '\n';
        } else {
            out << d.sourceName << " [" << d.code << "] " << d.message << '\n';
        }
    }
    return out.str();
}

bool hasDiagnosticCode(const diagnostics::DiagnosticBag& bag, const std::string& code) {
    for (const auto& d : bag.items()) {
        if (d.code == code) return true;
    }
    return false;
}

std::shared_ptr<const runtime::ProgramImage> buildAndLink(
    std::vector<compiler::SourceFile> sources,
    compiler::CompilationOptions opts = {}) {
    compiler::Compilation comp(std::move(sources), std::move(opts));
    auto build = comp.build();
    require(!build.diagnostics.hasErrors(),
        "Compilation failed:\n" + diagnosticsText(build.diagnostics));

    diagnostics::DiagnosticBag optDiag;
    optimization::Options optOpts;
    optOpts.level = optimization::Level::Basic;
    auto optimized = optimization::Optimizer{}.optimize(
        std::move(build.modules), optDiag, optOpts);
    require(!optDiag.hasErrors(), "Optimization failed:\n" + diagnosticsText(optDiag));

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcMods;
    bcMods.reserve(optimized.modules.size());
    for (const auto& m : optimized.modules) {
        bcMods.push_back(lowerer.lower(m));
    }
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcMods), linkErr);
    require(linked.has_value(), "Link failed: " + linkErr.message);
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

// ---------------------------------------------------------------------------
// 1. SourceText: #line / #line hidden / #line default / String+Comment Isolation / BOM / Overflow
// ---------------------------------------------------------------------------
void testSourceTextLineDirectivesAndBom() {
    const std::string generatedSrc = R"RS(module Test.LineMap;
#line 12 "ui/PanelVm.rscript"
int BadAssign() {
    int x = "type_mismatch";
    return x;
}
)RS";
    text::SourceText srcText(generatedSrc, "$generated/PanelVm.Reactive.g.rscript");
    compiler::Compilation comp({{srcText.name(), generatedSrc}});
    const auto build = comp.build();
    require(build.diagnostics.hasErrors(), "expected type mismatch diagnostic");
    const std::string formatted = diagnostics::formatDiagnostic(
        build.diagnostics.items().front(), srcText);
    require(
        formatted.find("ui/PanelVm.rscript:13:") != std::string::npos,
        "expected mapped path and line 13 in diagnostic: " + formatted);

    // #line inside /*...*/, //, "...", and @"..." must be ignored!
    const std::string ignoredDirectivesSrc = R"RS(module Test.IgnoredLines;
public int Probe() {
    /*
#line 900 "bogus/comment.rscript"
    */
    // #line 901 "bogus/line_comment.rscript"
    string s1 = "\n#line 902 \"bogus/str.rscript\"\n";
    string s2 = @"
#line 903 ""bogus/verbatim.rscript""
";
    return s1.Length + s2.Length;
}
)RS";
    text::SourceText ignoredText(ignoredDirectivesSrc, "ignored.rscript");
    require(
        ignoredText.lineDirectives().empty(),
        "#line directives inside comments or strings must be ignored");

    // Overflow guard on absurd line numbers (> kMaxLine)
    const std::string overflowSrc = "#line 99999999999999999999999999999 \"overflow.rscript\"\nint ok() { return 1; }\n";
    text::SourceText overflowText(overflowSrc, "overflow_test.rscript");
    const auto overflowMapped = overflowText.mappedLocation(overflowSrc.find("int ok()"));
    require(
        overflowMapped.position.line < 100000u,
        "#line integer overflow guard must not wrap to bogus line number");

    // #line hidden / #line N / #line default sequence points & UTF-8 BOM
    const std::string seqSrc = std::string("\xEF\xBB\xBF") + R"RS(module Test.SeqLines;
public int Compute(int v) {
#line hidden
    int glue = v + 10;
#line 42 "ui/Handwritten.rscript"
    int vis = glue * 2;
#line default
    return vis;
}
)RS";
    auto prog = buildAndLink({{"$generated/View.g.rscript", seqSrc}});
    bool sawHidden = false;
    bool sawMapped42 = false;
    for (const auto& mod : prog->modules()) {
        for (const auto& fn : mod.functions) {
            if (fn.name.find("Compute") == std::string::npos) continue;
            for (const auto& sp : fn.debugInfo.sequencePoints) {
                if (sp.hidden) sawHidden = true;
                if (!sp.hidden && sp.mappedPath == "ui/Handwritten.rscript" &&
                    sp.range.start.line == 41) {
                    sawMapped42 = true;
                }
            }
        }
    }
    require(sawHidden && sawMapped42, "expected both hidden and #line 42 sequence points");
}

// ---------------------------------------------------------------------------
// 2. Lexer / Parser: Verbatim, Tagged Strings, Trivia, Control Flow & Definite Assignment
// ---------------------------------------------------------------------------
void testLiteralsTriviaControlFlowAndDefiniteAssignment() {
    const std::string src = std::string("\xEF\xBB\xBF") + R"RS(
#nullable enable
#pragma warning disable 100
#region GeneratedLiterals
module Test.LiteralsAndFlow;

string __TagLiteral_ui(string raw) {
    return "[UI:" + raw + "]";
}

public int main(bool branch) {
    string path = @"C:\Root\Views\""Main"".xaml";
    string dsl = ui@"StackPanel { TextBlock }";
    int assigned;
    if (branch) {
        assigned = 10;
    } else {
        assigned = 20;
    }
    int loopSum = 0;
    for (int i = 0; i < 4; i = i + 1) {
        if (i == 2) { continue; }
        loopSum = loopSum + i;
    }
    return path.Length + dsl.Length + assigned + loopSum;
}
#endregion
)RS";
    auto prog = buildAndLink({{"LiteralsAndFlow.rscript", src}});
    runtime::Interpreter interp(prog);
    const auto r = interp.invoke("Test.LiteralsAndFlow::main", {true});
    require(r.succeeded && std::get<std::int64_t>(r.value) > 40,
        "verbatim/tagged/control-flow test failed: " + r.error.message);

    // Unhandled tag literal -> RS2690
    {
        compiler::Compilation badTag({{"BadTag.rscript", R"RS(
module Test.BadTag;
public string main() { return shader@"void main() {}"; }
)RS"}});
        require(hasDiagnosticCode(badTag.build().diagnostics, "RS2690"),
            "expected RS2690 for unhandled tagged string literal");
    }

    // Definite assignment failure: unassigned variable on else path
    {
        compiler::Compilation badAssign({{"Unassigned.rscript", R"RS(
module Test.Unassigned;
public int Broken(bool flag) {
    int x;
    if (flag) {
        x = 5;
    }
    return x;
}
)RS"}});
        require(badAssign.build().diagnostics.hasErrors(),
            "reading definitely unassigned variable must produce compiler error");
    }
}

// ---------------------------------------------------------------------------
// 3. OOP, Inheritance, Default Interface Methods, Sealed/Abstract, Generics & Pattern Matching
// ---------------------------------------------------------------------------
void testOopInterfacesGenericsAndPatterns() {
    const std::string src = R"RS(
module Test.OopGenericsPatterns;

public interface IFormatter {
    int Weight() { return 5; }
    int FormatScore(int input) { return input + Weight(); }
}

public abstract class BaseAgent : IFormatter {
    public int id;
    public BaseAgent(int i) { id = i; }
    public abstract int Eval();
    public virtual int Weight() { return 10; }
}

public sealed class CombatAgent : BaseAgent {
    public int power;
    public CombatAgent(int i, int p) : base(i) { power = p; }
    public override int Eval() { return id + power + ((IFormatter)this).FormatScore(3); }
}

public class GenericRepo<T> {
    private List<T> items = new List<T>();
    public void Add(T item) { items.Add(item); }
    public int Count() { return items.Count(); }
    public T First() { return items[0]; }
}

public int RunAll(int score) {
    CombatAgent ca = new CombatAgent(4, 20);
    IFormatter fmt = ca;
    int eval = ca.Eval(); // 4 + 20 + (3 + 10) = 37

    GenericRepo<int> repo = new GenericRepo<int>();
    repo.Add(eval);

    int tier = score switch {
        < 0 or > 100 => -1,
        >= 90 => 4,
        >= 60 and < 90 => 2,
        _ => 1
    };

    Dictionary<string, int> scores = new Dictionary<string, int>();
    scores.Set("tier", tier);
    return repo.First() + repo.Count() + scores.Get("tier"); // 37 + 1 + 4 = 42
}
)RS";
    auto prog = buildAndLink({{"OopGenericsPatterns.rscript", src}});
    runtime::Interpreter interp(prog);
    const auto r = interp.invoke("Test.OopGenericsPatterns::RunAll", {std::int64_t{95}});
    require(r.succeeded && std::get<std::int64_t>(r.value) == 42,
        "OOP/default-interface/generics/patterns execution mismatch: " + r.error.message);

    // Reject instantiating abstract class and subclassing sealed class
    {
        compiler::Compilation badAbstract({{"BadAbstract.rscript", R"RS(
module Test.BadOop;
public abstract class Abs { public int x; }
public sealed class FinalCls { public int y; }
public class BadSub : FinalCls {}
public Abs MakeAbs() { return new Abs(); }
)RS"}});
        require(badAbstract.build().diagnostics.hasErrors(),
            "abstract instantiation and sealed subclassing must fail");
    }
}

// ---------------------------------------------------------------------------
// 4. Speculative Lambda & Method-Group Overload Resolution + RS2473 Ambiguity + Rollback
// ---------------------------------------------------------------------------
void testSpeculativeLambdaOverloads() {
    auto prog = buildAndLink({
        {"Defs.rscript", R"RS(
module Test.OverloadDefs;
public delegate void Action0();
public delegate int IntOp(int x);
public delegate string StrOp(string s);

public class Dispatcher {
    public int sum = 0;
    public string text = "";

    public Dispatcher(Action0 a) { a(); }
    public Dispatcher(IntOp op) { sum = op(5); }
    public Dispatcher(int direct) { sum = direct; }

    public void Run(Action0 a) { a(); }
    public void Run(int v) { sum = sum + v; }

    public void Apply(Action0 a) { a(); }
    public void Apply(IntOp op) { sum = sum + op(7); }
    public void Apply(StrOp op) { text = op("ok"); }

    public void Bump() { sum = sum + 1; }
}
)RS"},
        {"Use.rscript", R"RS(
module Test.OverloadUse;
import Test.OverloadDefs;

public int main() {
    Dispatcher d1 = new Dispatcher(10);
    d1.Run(20);
    d1.Run(d1.Bump);
    d1.Apply(x => x * 2);      // rejects void Action0 & StrOp cleanly, picks IntOp without diagnostic leaks!
    d1.Apply(s => s + "!");    // picks StrOp across module boundary
    Dispatcher d2 = new Dispatcher(x => x + 3);
    if (d1.text != "ok!") { return -1; }
    return d1.sum + d2.sum; // (10 + 20 + 1 + 14) + 8 = 53
}
)RS"}
    });
    runtime::Interpreter interp(prog);
    const auto res = interp.invoke("Test.OverloadUse::main");
    require(res.succeeded && std::get<std::int64_t>(res.value) == 53,
        "speculative lambda overload disambiguation failed: " + res.error.message);

    // Ambiguous delegate overload must produce RS2473 cleanly
    {
        compiler::Compilation ambig({{"AmbigLambda.rscript", R"RS(
module Test.Ambig;
public delegate int OpA(int x);
public delegate long OpB(long x);
public class C {
    public static int F(OpA a) { return a(1); }
    public static long F(OpB b) { return b(1L); }
}
public int Bad() {
    return C.F(x => x);
}
)RS"}});
        const auto build = ambig.build();
        require(build.diagnostics.hasErrors() && hasDiagnosticCode(build.diagnostics, "RS2473"),
            "expected RS2473 for ambiguous lambda overload");
    }
}

// ---------------------------------------------------------------------------
// 5. Exact Numerics + using / [ValueLifecycle] Protocol
// ---------------------------------------------------------------------------
void testExactNumericsAndValueLifecycle() {
    auto prog = buildAndLink({{"NumericsLifecycle.rscript", R"RS(
module Test.NumLife;
import RealScript.Resource;

public static class Tracker {
    public static int Pins = 0;
}

public class ValueLifecycleAttribute { public ValueLifecycleAttribute() {} }

[ValueLifecycle]
public struct NativeGuard : IDisposable {
    public long Ptr;

    public static NativeGuard Alloc(long p) {
        Tracker.Pins = Tracker.Pins + 1;
        NativeGuard g = new NativeGuard();
        g.Ptr = p;
        return g;
    }

    public NativeGuard Retain() {
        if (Ptr != 0L) {
            Tracker.Pins = Tracker.Pins + 1;
        }
        return this;
    }

    public void Dispose() {
        if (Ptr != 0L) {
            Tracker.Pins = Tracker.Pins - 1;
            Ptr = 0L;
        }
    }
}

public int main() {
    byte b = 250;
    byte bw = unchecked((byte)(b + 10)); // 4
    sbyte sb = (sbyte)(-8);
    short sh = 120;
    ushort ush = 65500;
    uint u32 = 4000000000u;
    ulong u64 = 5000000000ul;
    char ch = 'A'; // 65
    float f = 2.5f;
    int flt = (int)(f * 4.0f); // 10

    using (NativeGuard g1 = NativeGuard.Alloc(77L)) {
        NativeGuard g2 = g1; // Retain() -> Pins == 2
        if (Tracker.Pins != 2) { return -100; }
        g2.Dispose();
        if (Tracker.Pins != 1) { return -200; }
    }
    if (Tracker.Pins != 0) { return -300; }

    return (int)bw + (int)sb + (int)sh + (u32 > (uint)ush ? 1 : 0) + (u64 > 10ul ? 2 : 0) + (int)ch + flt;
    // 4 - 8 + 120 + 1 + 2 + 65 + 10 = 194
}
)RS"}});
    runtime::Interpreter interp(prog);
    const auto res = interp.invoke("Test.NumLife::main");
    require(res.succeeded && std::get<std::int64_t>(res.value) == 194,
        "exact numerics + [ValueLifecycle] execution mismatch: " + res.error.message);
}

// ---------------------------------------------------------------------------
// 6. [StackOnly] Span<T>/ReadOnlySpan<T> Bounds Traps, Private Fields & Escape Matrix
// ---------------------------------------------------------------------------
void testStackOnlySpanAndAllEscapeGuards() {
    auto prog = buildAndLink({{"SpanExec.rscript", R"RS(
module Test.SpanExec;
import RealScript.Memory;

public int SumWindow(ReadOnlySpan<int> window) {
    int acc = 0;
    for (int i = 0; i < window.Length; i = i + 1) {
        acc = acc + window.Get(i);
    }
    return acc;
}

public int main() {
    int[] data = new int[] { 10, 20, 30, 40, 50 };
    Span<int> full = Span<int>.FromArray(data);
    Span<int> mid = full.Slice(1, 3);
    mid.Set(1, 300);
    ReadOnlySpan<int> ro = ReadOnlySpan<int>.FromArray(data).Slice(1, 3);
    return SumWindow(ro); // 20 + 300 + 40 = 360
}

public int BoundsTrapSlice() {
    int[] data = new int[] { 1, 2 };
    Span<int> s = Span<int>.FromArray(data);
    return s.Slice(1, 5).Get(0);
}

public int BoundsTrapGet() {
    int[] data = new int[] { 1, 2 };
    ReadOnlySpan<int> s = ReadOnlySpan<int>.FromArray(data);
    return s.Get(2);
}
)RS"}});
    runtime::Interpreter interp(prog);
    const auto okRes = interp.invoke("Test.SpanExec::main");
    require(okRes.succeeded && std::get<std::int64_t>(okRes.value) == 360,
        "Span<T> slice execution failed");

    const auto trapSlice = interp.invoke("Test.SpanExec::BoundsTrapSlice");
    require(!trapSlice.succeeded && trapSlice.error.code == runtime::ErrorCode::IndexOutOfRange,
        "Span.Slice out-of-bounds must trap with IndexOutOfRange");
    const auto trapGet = interp.invoke("Test.SpanExec::BoundsTrapGet");
    require(!trapGet.succeeded && trapGet.error.code == runtime::ErrorCode::IndexOutOfRange,
        "ReadOnlySpan.Get out-of-bounds must trap with IndexOutOfRange");

    // Private fields (__array / __offset / __length) cannot be accessed from user code
    {
        compiler::Compilation privComp({{"SpanPriv.rscript", R"RS(
module Test.SpanPriv;
import RealScript.Memory;
public int LeakArr() {
    int[] data = new int[] { 1, 2 };
    Span<int> s = Span<int>.FromArray(data);
    return s.__length;
}
)RS"}});
        require(privComp.build().diagnostics.hasErrors(),
            "Span<T> private fields must not be accessible externally");
    }

    // RS2692: class field AND transitive non-[StackOnly] struct holding Span<T>
    {
        compiler::Compilation cClass({{"ErrClassField.rscript", R"RS(
module Test.ErrClass;
import RealScript.Memory;
public class Holder { public Span<int> Leaked; }
)RS"}});
        require(hasDiagnosticCode(cClass.build().diagnostics, "RS2692"),
            "expected RS2692 for Span<T> in class field");

        compiler::Compilation cTransitive({{"ErrTransitiveStruct.rscript", R"RS(
module Test.ErrTransitive;
import RealScript.Memory;
public struct Wrapper { public Span<int> Leaked; }
public class LeakHolder { public Wrapper Leak; }
)RS"}});
        require(hasDiagnosticCode(cTransitive.build().diagnostics, "RS2692"),
            "expected RS2692 for transitively [StackOnly] struct in class field");
    }

    // RS2693: closure capture
    {
        compiler::Compilation cClosure({{"ErrClosure.rscript", R"RS(
module Test.ErrClosure;
import RealScript.Memory;
public delegate int Reader();
public int Bad() {
    int[] arr = new int[] { 1, 2 };
    Span<int> s = Span<int>.FromArray(arr);
    Reader r = () => s.Get(0);
    return r();
}
)RS"}});
        require(hasDiagnosticCode(cClosure.build().diagnostics, "RS2693"),
            "expected RS2693 for Span<T> captured in closure");
    }

    // RS2694: boxing to interface or object
    {
        compiler::Compilation cBox({{"ErrBox.rscript", R"RS(
module Test.ErrBox;
public class StackOnlyAttribute { public StackOnlyAttribute() {} }
public interface IBoxTarget { int Len(); }
[StackOnly]
public struct NativeView : IBoxTarget {
    public int N;
    public int Len() { return N; }
}
public IBoxTarget Bad() {
    NativeView v = new NativeView();
    IBoxTarget boxed = v;
    return boxed;
}
)RS"}});
        require(hasDiagnosticCode(cBox.build().diagnostics, "RS2694"),
            "expected RS2694 for boxing [StackOnly] struct");
    }

    // RS2695: array of [StackOnly] elements
    {
        compiler::Compilation cArray({{"ErrArray.rscript", R"RS(
module Test.ErrArray;
import RealScript.Memory;
public int Bad() {
    Span<int>[] arr = new Span<int>[1];
    return arr.Length;
}
)RS"}});
        require(hasDiagnosticCode(cArray.build().diagnostics, "RS2695"),
            "expected RS2695 for array of [StackOnly] elements");
    }

    // RS2696: async or sequence local
    {
        compiler::Compilation cAsync({{"ErrAsync.rscript", R"RS(
module Test.ErrAsync;
import RealScript.Memory;
import RealScript.Async;
public async Awaitable<int> Bad() {
    int[] a = new int[] { 1 };
    Span<int> s = Span<int>.FromArray(a);
    await Awaitable.Completed();
    return s.Get(0);
}
)RS"}});
        require(hasDiagnosticCode(cAsync.build().diagnostics, "RS2696"),
            "expected RS2696 for [StackOnly] local in async function");
    }

    // RS2697: ref/out parameter
    {
        compiler::Compilation cRefParam({{"ErrRefParam.rscript", R"RS(
module Test.ErrRef;
import RealScript.Memory;
public void Bad(ref Span<int> s) {}
)RS"}});
        require(hasDiagnosticCode(cRefParam.build().diagnostics, "RS2697"),
            "expected RS2697 for [StackOnly] ref parameter");
    }
}

// ---------------------------------------------------------------------------
// 7. CompilerSession, ModulePackRegistry (Memory/Weak), ICompilerMacroPlugin & Multi-File Partial
// ---------------------------------------------------------------------------
class ReactiveMacroPlugin final : public compiler::ICompilerMacroPlugin {
public:
    std::string name() const override { return "ReactiveMacroPlugin"; }

    void expand(
        const compiler::MacroExpansionContext& context,
        std::vector<compiler::SourceFile>& outGeneratedSources,
        diagnostics::DiagnosticBag&) const override {
        for (const auto& unit : context.parsedUnits) {
            if (!unit.syntaxTree || !unit.sourceText) continue;
            for (const auto& cls : unit.syntaxTree->classes) {
                bool hasObs = false;
                std::ostringstream gen;
                gen << "module " << unit.moduleName << ";\n";
                gen << "public partial class " << cls.identifierToken.text;
                if (!cls.typeParameters.empty()) {
                    gen << "<";
                    for (std::size_t ti = 0; ti < cls.typeParameters.size(); ++ti) {
                        if (ti > 0) gen << ", ";
                        gen << cls.typeParameters[ti].text;
                    }
                    gen << ">";
                }
                gen << " {\n";
                for (const auto& field : cls.fields) {
                    bool isObs = false;
                    for (const auto& list : field.attributes) {
                        for (const auto& a : list.attributes) {
                            if (a.nameToken.text == "Observable") isObs = true;
                        }
                    }
                    if (!isObs) continue;
                    hasObs = true;
                    std::string prop = field.identifierToken.text;
                    if (!prop.empty() && prop[0] == '_') prop.erase(0, 1);
                    if (!prop.empty()) {
                        prop[0] = static_cast<char>(
                            std::toupper(static_cast<unsigned char>(prop[0])));
                    }
                    const auto line1 =
                        unit.sourceText->linePosition(field.identifierToken.span.start).line + 1;
                    gen << "#line " << line1 << " \"" << unit.sourcePath << "\"\n";
                    gen << "    public " << field.type.name.text << " " << prop << " {\n";
                    gen << "        get { return this." << field.identifierToken.text << "; }\n";
                    gen << "        set { this." << field.identifierToken.text << " = value; }\n";
                    gen << "    }\n#line default\n";
                }
                gen << "}\n";
                if (hasObs) {
                    outGeneratedSources.push_back({
                        "$generated/" + cls.identifierToken.text + ".Reactive.g.rscript",
                        gen.str()});
                }
            }
        }
    }
};

void testPacksSessionMacrosAndPartials() {
    // ModulePackRegistry builtin checks + disabling + custom pack + CompilerSession isolation
    const auto& defaults = compiler::ModulePackRegistry::builtinDefaults();
    require(
        defaults.findPack("RealScript.Memory") != nullptr &&
            defaults.findPack("RealScript.Weak") != nullptr,
        "builtin defaults must provide RealScript.Memory and RealScript.Weak");

    auto sessionA = std::make_shared<compiler::CompilerSession>();
    auto sessionB = std::make_shared<compiler::CompilerSession>();
    compiler::CompilationOptions optsNoMath;
    optsNoMath.session = sessionA;
    optsNoMath.disabledPacks.insert("RealScript.Math");
    compiler::Compilation compNoMath({{"NoMath.rscript", R"RS(
module App.NoMath;
import RealScript.Math;
public int main() { return 0; }
)RS"}}, optsNoMath);
    require(compNoMath.build().diagnostics.hasErrors(), "disabled RealScript.Math must not resolve");

    // Multi-file partial class/struct, merged generic where constraints, generic partial method & macro #line
    compiler::CompilationOptions macroOpts;
    macroOpts.session = sessionB;
    macroOpts.macroPlugins.push_back(std::make_shared<ReactiveMacroPlugin>());
    static const std::string kCustomPack = R"RS(
module Custom.Pack;
public static class Constants { public static int Bias() { return 5; } }
)RS";
    macroOpts.extraPacks.push_back(compiler::ModulePack{
        "Custom.Pack",
        "$pack/Custom.Pack.rscript",
        []() -> const std::string& { return kCustomPack; },
        nullptr,
        false});

    auto prog = buildAndLink({
        {"Part1.rscript", R"RS(
module Test.PartialMerged;
import RealScript.Weak;
import Custom.Pack;

public class ObservableAttribute { public ObservableAttribute() {} }
public interface IVal { int Val(); }
public class Item : IVal { public int v; public Item(int x) { v = x; } public int Val() { return v; } }

public partial class Holder<T> where T : class {
    [Observable]
    private int _score = 10;
    public partial int Transform<U>(T input, U extra) where U : IVal;
    partial void OptionalHook();
}
)RS"},
        {"Part2.rscript", R"RS(
module Test.PartialMerged;

public partial class Holder<T> where T : IVal {
    public partial int Transform<U>(T input, U extra) where U : IVal {
        OptionalHook();
        return input.Val() + extra.Val();
    }
}

public int main() {
    Holder<Item> h = new Holder<Item>();
    h.Score = 30;
    int t = h.Transform<Item>(new Item(2), new Item(3));
    WeakRef<Item> weak = WeakRef<Item>.Empty();
    return h.Score + Constants.Bias() + t + (weak.HasHandle() ? 0 : 1);
}
)RS"}
    }, std::move(macroOpts));
    require(
        sessionA->parsedUnitCacheSize() != sessionB->parsedUnitCacheSize() ||
            sessionB->parsedUnitCacheSize() >= 2,
        "CompilerSession caches should remain isolated");

    // Verify macro-generated #line preserved in compiled debug info
    bool foundMacroLine = false;
    for (const auto& mod : prog->modules()) {
        for (const auto& fn : mod.functions) {
            if (fn.name.find("set_Score") == std::string::npos &&
                fn.name.find("get_Score") == std::string::npos) {
                continue;
            }
            for (const auto& sp : fn.debugInfo.sequencePoints) {
                if (sp.mappedPath == "Part1.rscript") {
                    foundMacroLine = true;
                }
            }
        }
    }
    require(foundMacroLine, "macro-generated partial property must preserve #line mappedPath 'Part1.rscript'");

    runtime::Interpreter interp(prog);
    const auto res = interp.invoke("Test.PartialMerged::main");
    require(res.succeeded && std::get<std::int64_t>(res.value) >= 35,
        "merged partial + macro + WeakRef execution failed: " + res.error.message);

    // Mismatched partial diagnostics: RS4020, RS4021, RS4022, RS4023, RS4024
    {
        compiler::Compilation c4020({{"P4020.rscript", R"RS(
module Test.P4020;
public partial class A { public partial int MissingImpl(); }
)RS"}});
        require(hasDiagnosticCode(c4020.build().diagnostics, "RS4020"), "expected RS4020");

        compiler::Compilation c4021({
            {"P4021a.rscript", "module Test.P4021; public partial class Gen<T> {}"},
            {"P4021b.rscript", "module Test.P4021; public partial class Gen<U> {}"},
        });
        require(hasDiagnosticCode(c4021.build().diagnostics, "RS4021"), "expected RS4021");

        compiler::Compilation c4022({
            {"P4022a.rscript", "module Test.P4022; public partial class Gen<T> where T : class {}"},
            {"P4022b.rscript", "module Test.P4022; public partial class Gen<T> where T : struct {}"},
        });
        require(hasDiagnosticCode(c4022.build().diagnostics, "RS4022"), "expected RS4022");

        compiler::Compilation c4023({
            {"P4023a.rscript", "module Test.P4023; public partial class Dup { partial void M(); }"},
            {"P4023b.rscript", "module Test.P4023; public partial class Dup { partial void M(); }"},
        });
        require(hasDiagnosticCode(c4023.build().diagnostics, "RS4023"), "expected RS4023");

        compiler::Compilation c4024({
            {"P4024a.rscript", "module Test.P4024; public partial class Dup { partial void M() {} }"},
            {"P4024b.rscript", "module Test.P4024; public partial class Dup { partial void M() {} }"},
        });
        require(hasDiagnosticCode(c4024.build().diagnostics, "RS4024"), "expected RS4024");
    }
}

struct FrontendTempDirGuard {
    std::filesystem::path dir;
    explicit FrontendTempDirGuard(const std::string& name) {
        dir = std::filesystem::temp_directory_path() / ("rs_frontend_" + name);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
    }
    ~FrontendTempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    FrontendTempDirGuard(const FrontendTempDirGuard&) = delete;
    FrontendTempDirGuard& operator=(const FrontendTempDirGuard&) = delete;
};

void writeFrontendTextFile(const std::filesystem::path& p, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    require(out.good(), "failed to open file for writing: " + p.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(out.good(), "failed to write file: " + p.string());
}

std::string jsonEscapePath(const std::filesystem::path& p) {
    std::string raw = p.generic_string();
    std::string escaped;
    escaped.reserve(raw.size() + 8);
    for (char ch : raw) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

int runFrontendProcess(const std::string& commandLine) {
#ifdef _WIN32
    const std::string wrapped = "\"" + commandLine + "\"";
    return std::system(wrapped.c_str());
#else
    return std::system(commandLine.c_str());
#endif
}

void testBlittableSchemaEvolutionProjectConfigAndMacroCache() {
    // 1. [Blittable] struct layout offsets & ManagedHeap packed stride parity (incl. math types).
    const std::string validBlitSrc = R"RS(
module Test.BlitLayout;
import RealScript.Math;

[Blittable]
public struct InnerPair {
    public byte tag;
    public int id;
}

[Blittable]
public struct PackedFrame {
    public byte kind;
    public Vec2f pos2;
    public Vec3f pos3;
    public Quatf rotf;
    public Vec2 fxPos2;
    public Vec3 fxPos3;
    public Quat fxRot;
    public Fixed16 f16;
    public InnerPair inner;
    public byte tail;
}

public class TaggedRecord {
    [FieldTag(101)]
    [FormerlySerializedAs("v1Hp")]
    [FormerlySerializedAs("v2Hp")]
    public int hp = 100;

    [FieldTag(202)]
    public string label = "ok";
}

public int TouchPacked() {
    var p = new PackedFrame();
    p.kind = (byte)7;
    p.tail = (byte)9;
    return (int)p.kind + (int)p.tail;
}
)RS";
    auto prog = buildAndLink({{"BlitLayout.rscript", validBlitSrc}});

    const bytecode::Module* targetBc = nullptr;
    for (const auto& m : prog->modules()) {
        if (m.name == "Test.BlitLayout") {
            targetBc = &m;
            break;
        }
    }
    require(targetBc != nullptr, "expected Test.BlitLayout bytecode module");

    const semantic::TypeSymbol* packedSym = nullptr;
    const semantic::TypeSymbol* innerSym = nullptr;
    for (const auto& t : targetBc->types) {
        if (t.name == "PackedFrame") packedSym = &t;
        if (t.name == "InnerPair") innerSym = &t;
    }
    require(packedSym != nullptr && innerSym != nullptr, "expected PackedFrame and InnerPair types");
    require(packedSym->blittable && innerSym->blittable, "expected TypeSymbol::blittable == true");

    std::string layoutErr;
    const auto innerLayout = semantic::computeBlittableStructLayout(*innerSym, targetBc->types, &layoutErr);
    require(innerLayout.has_value(), "InnerPair computeBlittableStructLayout failed: " + layoutErr);
    require(innerLayout->alignBytes == 4u && innerLayout->strideBytes == 8u, "InnerPair size/align mismatch");
    require(innerLayout->fields.size() == 2u &&
                innerLayout->fields[0].offsetBytes == 0u &&
                innerLayout->fields[1].offsetBytes == 4u,
        "InnerPair field offsets mismatch");

    const auto packedLayout = semantic::computeBlittableStructLayout(*packedSym, targetBc->types, &layoutErr);
    require(packedLayout.has_value(), "PackedFrame computeBlittableStructLayout failed: " + layoutErr);
    require(packedLayout->fields.size() == 10u, "expected 10 fields in PackedFrame");
    // Byte(0,1), Vec2f(4,8), Vec3f(12,12), Quatf(24,16), Vec2(40,8), Vec3(48,12), Quat(60,16),
    // Fixed16(80,8) -> align 8! InnerPair(88,8), tail(96,1) -> total aligned to 8 = 104!
    require(packedLayout->fields[0].offsetBytes == 0u &&
                packedLayout->fields[1].offsetBytes == 4u &&
                packedLayout->fields[2].offsetBytes == 12u &&
                packedLayout->fields[3].offsetBytes == 24u &&
                packedLayout->fields[4].offsetBytes == 40u &&
                packedLayout->fields[5].offsetBytes == 48u &&
                packedLayout->fields[6].offsetBytes == 60u &&
                packedLayout->fields[7].offsetBytes == 80u &&
                packedLayout->fields[8].offsetBytes == 88u &&
                packedLayout->fields[9].offsetBytes == 96u &&
                packedLayout->alignBytes == 8u &&
                packedLayout->strideBytes == 104u,
        "PackedFrame blittable offset/stride/alignment mismatch");

    // Verify ManagedHeap packed struct registration parity with computeBlittableStructLayout.
    {
        runtime::EngineRuntime rt(prog);
        auto heap = rt.heap();
        require(heap != nullptr, "EngineRuntime heap missing");

        const auto packedTid = semantic::stableTypeId("Test.BlitLayout::PackedFrame");
        require(runtime::isBlittableStructType(packedTid), "expected PackedFrame registered as blittable");
        const auto zeroFixed = runtime::Value{runtime::FixedValue{0}};
        const auto zeroFloat = runtime::Value{runtime::FloatValue{0.0f}};
        const auto v2f = runtime::Value{runtime::Vec2fValue{0.0f, 0.0f}};
        const auto v3f = runtime::Value{runtime::Vec3fValue{0.0f, 0.0f, 0.0f}};
        const auto qf = runtime::Value{runtime::QuatfValue{0.0f, 0.0f, 0.0f, 1.0f}};
        const auto v2 = runtime::Value{runtime::Vec2Value{0, 0}};
        const auto v3 = runtime::Value{runtime::Vec3Value{0, 0, 0}};
        const auto q = runtime::Value{runtime::QuatValue{0, 0, 0, 1}};
        const auto f16 = runtime::Value{runtime::Fixed16Value{0}};
        const std::vector<runtime::Value> innerFields{
            runtime::Value{runtime::ByteValue{0}}, runtime::Value{std::int64_t{0}}};
        const auto innerVal = runtime::Value{runtime::makeStructValueFromFields(
            semantic::stableTypeId("Test.BlitLayout::InnerPair"), innerFields)};
        const std::vector<runtime::Value> pfFields{
            runtime::Value{runtime::ByteValue{0}}, v2f, v3f, qf, v2, v3, q, f16, innerVal,
            runtime::Value{runtime::ByteValue{0}}};
        const auto pfVal = runtime::Value{runtime::makeStructValueFromFields(packedTid, pfFields)};
        runtime::RuntimeError arrErr;
        const auto arrRef = heap->allocateTypedArray(
            semantic::stableTypeId("Test.BlitLayout::PackedFrame[]"),
            semantic::PrimitiveType::Struct,
            packedTid,
            2u,
            pfVal,
            &arrErr);
        require(arrRef.has_value(), "allocateTypedArray(PackedFrame[]) failed: " + arrErr.message);
        runtime::PackedArrayFastView view{};
        require(heap->tryBindPackedArrayFastView(*arrRef, view) &&
                    view.encoding == runtime::PackedElemEncoding::BlittableStruct &&
                    view.strideBytes == packedLayout->strideBytes,
            "ManagedHeap PackedFrame stride must match computeBlittableStructLayout");
        (void)zeroFixed;
        (void)zeroFloat;
    }

    // 2. [FieldTag(N)] & multi-[FormerlySerializedAs] .rsbc round-trip + SectionCount == 8 / v0.7 unchanged.
    {
        const auto encoded = bytecode::encodeModule(*targetBc);
        require(encoded.size() >= 16u &&
                    encoded[0] == 'R' && encoded[1] == 'S' && encoded[2] == 'B' && encoded[3] == 'C',
            "encoded .rsbc header magic mismatch");
        const auto secCount =
            static_cast<std::uint32_t>(encoded[12]) |
            (static_cast<std::uint32_t>(encoded[13]) << 8u) |
            (static_cast<std::uint32_t>(encoded[14]) << 16u) |
            (static_cast<std::uint32_t>(encoded[15]) << 24u);
        require(secCount == 8u, "expected .rsbc SectionCount == 8");

        bytecode::Module decoded;
        diagnostics::DiagnosticBag decDiag;
        require(bytecode::decodeModule(encoded, decoded, decDiag) && !decDiag.hasErrors(),
            "decodeModule BlitLayout failed: " + diagnosticsText(decDiag));
        require(decoded.version.major == 0u && decoded.version.minor == 7u, "expected version 0.7");

        const semantic::TypeSymbol* decTagged = nullptr;
        const semantic::TypeSymbol* decPacked = nullptr;
        for (const auto& t : decoded.types) {
            if (t.name == "TaggedRecord") decTagged = &t;
            if (t.name == "PackedFrame") decPacked = &t;
        }
        require(decTagged != nullptr && decPacked != nullptr, "decoded types missing");
        require(decPacked->blittable, "decoded PackedFrame::blittable must persist across .rsbc");
        require(decTagged->fields.size() == 2u, "expected 2 fields in TaggedRecord");
        require(decTagged->fields[0].fieldTag == std::optional<std::int32_t>{101} &&
                    decTagged->fields[0].formerlySerializedAs.size() == 2u &&
                    decTagged->fields[0].formerlySerializedAs[0] == "v1Hp" &&
                    decTagged->fields[0].formerlySerializedAs[1] == "v2Hp" &&
                    decTagged->fields[1].fieldTag == std::optional<std::int32_t>{202},
            "decoded FieldTag / multi-FormerlySerializedAs mismatch");
    }

    // 3. Negative RS2698 & RS2699 diagnostics.
    {
        compiler::Compilation cRefField({{"BadBlitRef.rscript",
            "module Test.BadBlitRef; [Blittable] public struct Bad { public string s; }"}});
        require(hasDiagnosticCode(cRefField.build().diagnostics, "RS2698"), "expected RS2698 for string field");

        compiler::Compilation cClassBlit({{"BadBlitCls.rscript",
            "module Test.BadBlitCls; [Blittable] public class Bad { public int x; }"}});
        require(hasDiagnosticCode(cClassBlit.build().diagnostics, "RS2698"), "expected RS2698 for class");

        compiler::Compilation cEmptyBlit({{"BadBlitEmpty.rscript",
            "module Test.BadBlitEmpty; [Blittable] public struct Empty {}"}});
        require(hasDiagnosticCode(cEmptyBlit.build().diagnostics, "RS2698"), "expected RS2698 for empty struct");

        compiler::Compilation cDupTag({{"BadDupTag.rscript",
            "module Test.BadDupTag; public class C { [FieldTag(7)] public int a; [FieldTag(7)] public int b; }"}});
        require(hasDiagnosticCode(cDupTag.build().diagnostics, "RS2699"), "expected RS2699 for duplicate FieldTag");

        compiler::Compilation cNegTag({{"BadNegTag.rscript",
            "module Test.BadNegTag; public class C { [FieldTag(-1)] public int a; }"}});
        require(hasDiagnosticCode(cNegTag.build().diagnostics, "RS2699"), "expected RS2699 for negative FieldTag");

        compiler::Compilation cStaticTag({{"BadStatTag.rscript",
            "module Test.BadStatTag; public class C { [FieldTag(3)] public static int a = 1; }"}});
        require(hasDiagnosticCode(cStaticTag.build().diagnostics, "RS2699"), "expected RS2699 for static FieldTag");
    }

    // 4. realscript.project.json + C11 kernel_fixture_macro_plugin + ModuleBuildCache invalidation + rsc --project.
#ifdef REALSCRIPT_TEST_MACRO_PLUGIN_PATH
    const std::filesystem::path pluginDllPath = REALSCRIPT_TEST_MACRO_PLUGIN_PATH;
    require(std::filesystem::exists(pluginDllPath),
        "REALSCRIPT_TEST_MACRO_PLUGIN_PATH missing: " + pluginDllPath.string());

    FrontendTempDirGuard guard("project_macro_cache");
    const auto cfgPath = guard.dir / "realscript.project.json";
    const auto srcPath = guard.dir / "src" / "Hero.rscript";
    const auto cacheDir = guard.dir / ".rs_cache";

    writeFrontendTextFile(cfgPath,
        "{\n"
        "  \"macroPlugins\": [ \"" + jsonEscapePath(pluginDllPath) + "\" ]\n"
        "}\n");

    const std::string heroV1 = R"RS(
module Test.ProjMacro;

public delegate void HeroDamageHandler(int amount);

[AutoGetter("hp")]
[QualifiedProbe("HeroType")]
public partial class Hero {
    [QualifiedProbe("HeroField")]
    public int hp = 88;
    [QualifiedProbe("HeroEvent")]
    public event HeroDamageHandler Damaged;
    [QualifiedProbe("HeroProp")]
    public int Armor { get { return 7; } }
    [QualifiedProbe("HeroMethod")]
    public void Heal(int v) { hp = hp + v; }
}

[QualifiedProbe("StructType")]
public struct StatVec {
    [QualifiedProbe("StructField")]
    public int x;
}

public int Run() {
    var h = new Hero();
    if (probe_HeroType() != "Hero") return -1;
    if (probe_HeroField() != "Hero.hp") return -2;
    if (probe_HeroEvent() != "Hero.Damaged") return -3;
    if (probe_HeroProp() != "Hero.Armor") return -4;
    if (probe_HeroMethod() != "Hero.Heal") return -5;
    if (probe_StructType() != "StatVec") return -6;
    if (probe_StructField() != "StatVec.x") return -7;
    return h.GetHp() + plugin_magic(12);
}
)RS";
    writeFrontendTextFile(srcPath, heroV1);

    const auto discovered = compiler::discoverProjectConfigFile(srcPath);
    require(discovered.has_value() && std::filesystem::equivalent(*discovered, cfgPath),
        "discoverProjectConfigFile failed to locate realscript.project.json");
    diagnostics::DiagnosticBag cfgDiag;
    const auto loadedCfg = compiler::loadProjectConfigFile(*discovered, &cfgDiag);
    require(loadedCfg.has_value() && !cfgDiag.hasErrors() &&
                loadedCfg->nativeMacroPluginPaths.size() == 1u,
        "loadProjectConfigFile failed");
    const auto directPlugin = compiler::loadNativeCompilerMacroPlugin(pluginDllPath, &cfgDiag);
    require(directPlugin != nullptr && !cfgDiag.hasErrors(),
        "loadNativeCompilerMacroPlugin failed");

    compiler::ModuleBuildCache cache(cacheDir);
    compiler::ModuleBuildCacheOptions cacheOpts;
    cacheOpts.cacheDirectory = cacheDir;

    const auto build1 = cache.build({{srcPath.string(), heroV1}}, cacheOpts);
    require(build1.succeeded(), "Project Macro build1 failed: " + diagnosticsText(build1.diagnostics));
    require(build1.statistics.cacheMisses >= 1u, "expected cold cache miss on build1");
    {
        runtime::RuntimeError linkErr;
        auto linked = runtime::ProgramImage::link(build1.bytecodeModules, linkErr);
        require(linked.has_value(), "link build1 failed: " + linkErr.message);
        auto macroProg = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
        runtime::Interpreter interp(macroProg);
        const auto r = interp.invoke("Test.ProjMacro::Run", {});
        require(r.succeeded && std::get<std::int64_t>(r.value) == (88 + 12 + 1337),
            "expected AutoGetter + plugin_magic(12) == 1437");
    }

    const auto build2 = cache.build({{srcPath.string(), heroV1}}, cacheOpts);
    require(build2.succeeded(), "build2 failed: " + diagnosticsText(build2.diagnostics));
    require(build2.statistics.cacheHits >= 1u && build2.statistics.cacheMisses == 0u,
        "expected warm cache hit on unchanged build2");

    // Modify source -> invalidates cache fingerprint cleanly.
    std::string heroV2 = heroV1;
    const auto pos = heroV2.find("hp = 88;");
    require(pos != std::string::npos, "missing replacement anchor");
    heroV2.replace(pos, 8, "hp = 99;");
    writeFrontendTextFile(srcPath, heroV2);
    const auto build3 = cache.build({{srcPath.string(), heroV2}}, cacheOpts);
    require(build3.succeeded(), "build3 failed: " + diagnosticsText(build3.diagnostics));
    require(build3.statistics.cacheMisses >= 1u, "expected cache invalidation on edited source");

    // Touch project config (fingerprint change) -> invalidates cache cleanly too!
    writeFrontendTextFile(cfgPath,
        "{\n"
        "  \"disabledPacks\": [],\n"
        "  \"macroPlugins\": [ \"" + jsonEscapePath(pluginDllPath) + "\" ]\n"
        "}\n");
    const auto build4 = cache.build({{srcPath.string(), heroV2}}, cacheOpts);
    require(build4.succeeded(), "build4 failed: " + diagnosticsText(build4.diagnostics));
    require(build4.statistics.cacheMisses >= 1u,
        "expected ModuleBuildCache invalidation when realscript.project.json fingerprint changes");

#ifdef REALSCRIPT_RSC_PATH
    const std::string rscExe = REALSCRIPT_RSC_PATH;
    if (!rscExe.empty()) {
        const auto cliLog = guard.dir / "rsc_project.log";
        const std::string cmd =
            "\"" + rscExe + "\" \"" + srcPath.string() + "\" --project \"" + cfgPath.string() +
            "\" --check > \"" + cliLog.string() + "\" 2>&1";
        const int rc = runFrontendProcess(cmd);
        require(rc == 0, "rsc --project --check failed on macro plugin project");
    }
#endif
#endif
}

// ---------------------------------------------------------------------------
// 9. L0 C# Base Syntax Primitives (Strings, => Members, : this(...), Iface:Iface, Generics, Lambdas)
// ---------------------------------------------------------------------------
void testCSharpBaseSyntaxPrimitives() {
    // A. Verbatim & verbatim-interpolated strings, => members, : this(...), interface inheritance,
    //    arity-overloaded generics & delegates, typed/discard/block lambdas & reference capture
    {
        const std::string src = R"RS(
module Kernel.CSharpBase;

public delegate void Action();
public delegate void Action<T>(T x);
public delegate R Func<R>();
public delegate R Func<T, R>(T x);

public interface IBaseCounter {
    int BaseValue();
}

public interface IDerivedCounter : IBaseCounter {
    int DerivedValue();
}

public class ArityBox {
    public int tag = 10;
}

public class ArityBox<T> {
    public T item;
    public ArityBox(T initial) => item = initial;
}

public class MultiCounter : IDerivedCounter {
    public int value = 0;

    public MultiCounter() : this(5) {}
    public MultiCounter(int start) => value = start;

    public int Twice => value * 2;

    public int BaseValue() => value;
    public int DerivedValue() => value + 100;
}

public class PropHolder {
    private int inner = 3;
    public int Doubled {
        get => inner * 2;
        set => inner = value;
    }
}

public interface IArityPack {
    int Tag();
}

public interface IArityPack<T> {
    T Peek();
}

public class IntPack : IArityPack, IArityPack<int> {
    public int Tag() => 1;
    public int Peek() => 2;
}

public int TestStringsAndChaining() {
    string raw = @"a\b""c";
    int n = 42;
    string interp1 = $@"v={n}\x";
    string interp2 = @$"v={n}\y";
    MultiCounter mc = new MultiCounter();
    IDerivedCounter dc = mc;
    PropHolder ph = new PropHolder();
    ph.Doubled = 9;
    ArityBox b0 = new ArityBox();
    ArityBox<int> b1 = new ArityBox<int>(4);
    int cap = 1;
    Func<int, int> inc = (int x) => {
        cap = cap + 2;
        return x + cap;
    };
    int r1 = inc(10);
    int r2 = inc(10);
    Func<int, int, int> pickSecond = (_, y) => y * 2;
    if (raw != "a\\b\"c" || interp1 != "v=42\\x" || interp2 != "v=42\\y") return -1;
    return mc.Twice + dc.BaseValue() + dc.DerivedValue() + ph.Doubled + b0.tag + b1.item + r1 + r2 + cap + pickSecond(999, 7);
}
)RS";
        const std::string srcWithOverload = [&]() {
            std::string s = src;
            const auto pos = s.find("public delegate R Func<T, R>(T x);");
            s.insert(pos + std::strlen("public delegate R Func<T, R>(T x);"),
                "\npublic delegate R Func<T1, T2, R>(T1 a, T2 b);");
            return s;
        }();
        compiler::Compilation comp({{"cs_base.rscript", srcWithOverload}});
        auto build = comp.build();
        require(!build.diagnostics.hasErrors(),
            "C# base syntax compile failed:\n" + diagnosticsText(build.diagnostics));

        diagnostics::DiagnosticBag optDiag;
        auto optimized = optimization::Optimizer{}.optimize(std::move(build.modules), optDiag);
        require(!optDiag.hasErrors(), "C# base syntax optimization failed");
        bytecode::Lowerer lowerer;
        std::vector<bytecode::Module> bcMods;
        for (const auto& m : optimized.modules) bcMods.push_back(lowerer.lower(m));
        runtime::RuntimeError linkErr;
        auto linked = runtime::ProgramImage::link(std::move(bcMods), linkErr);
        require(linked.has_value(), "Link failed: " + linkErr.message);
        auto prog = std::make_shared<const runtime::ProgramImage>(std::move(*linked));
        runtime::Interpreter interp(prog);
        const auto res = interp.invoke("Kernel.CSharpBase::TestStringsAndChaining");
        require(res.succeeded && std::holds_alternative<std::int64_t>(res.value) &&
                std::get<std::int64_t>(res.value) == 199,
            "TestStringsAndChaining expected 199");
    }

    // B. : this(...) constructor cycle -> RS2535
    {
        compiler::Compilation cCyc({{"CtorCycle.rscript", R"RS(
module Kernel.CtorCycle;
public class C {
    public C() : this(1) {}
    public C(int x) : this() {}
}
)RS"}});
        require(hasDiagnosticCode(cCyc.build().diagnostics, "RS2535"),
            "Expected RS2535 for cyclic ': this(...)' constructors");
    }

    // C. Cyclic interface inheritance -> RS2476
    {
        compiler::Compilation iCyc({{"IfaceCycle.rscript", R"RS(
module Kernel.IfaceCycle;
public interface IA : IB { int A(); }
public interface IB : IA { int B(); }
)RS"}});
        require(hasDiagnosticCode(iCyc.build().diagnostics, "RS2476"),
            "Expected RS2476 for cyclic interface inheritance");
    }

    // D. Generic arity mismatch (RS8501) and constraint diagnostics (RS8531..RS8533)
    {
        compiler::Compilation badArity({{"GenArity.rscript", R"RS(
module Kernel.GenArity;
public class Box {}
public class Box<T> { public T val; }
public void Bad() { Box<int, int> x = null; }
)RS"}});
        require(hasDiagnosticCode(badArity.build().diagnostics, "RS8501"),
            "Expected RS8501 for mismatched arity on arity-overloaded generic type");

        compiler::Compilation c8531({{"G8531.rscript", R"RS(
module Kernel.G8531;
public class Bad<T> where U : class { public T v; }
)RS"}});
        require(hasDiagnosticCode(c8531.build().diagnostics, "RS8531"),
            "Expected RS8531 for unknown constraint type parameter");

        compiler::Compilation c8532({{"G8532.rscript", R"RS(
module Kernel.G8532;
public class Bad<T> where T : class, class { public T v; }
)RS"}});
        require(hasDiagnosticCode(c8532.build().diagnostics, "RS8532"),
            "Expected RS8532 for duplicate constraint");

        compiler::Compilation c8533({{"G8533.rscript", R"RS(
module Kernel.G8533;
public class Bad<T> where T : class, struct { public T v; }
)RS"}});
        require(hasDiagnosticCode(c8533.build().diagnostics, "RS8533"),
            "Expected RS8533 for conflicting class/struct constraints");
    }
}

// ---------------------------------------------------------------------------
// 10. Built-in Standard Reactive Macros ([Signal] & [RelayCommand], 3-Way Partial)
// ---------------------------------------------------------------------------
void testStandardReactiveMacros() {
    // A. 3-way partial class: View.g.rs (XAML) + ViewModel.rs + StandardReactiveMacroPlugin
    //    coexisting alongside ReactiveMacroPlugin ([Observable]) with zero collision.
    {
        compiler::CompilationOptions opts;
        opts.macroPlugins.push_back(std::make_shared<ReactiveMacroPlugin>());
        opts.macroPlugins.push_back(compiler::createStandardReactiveMacroPlugin());

        auto prog = buildAndLink({
            {"SaveDialogViewModel.g.rs", R"RS(
module Demo.ReactiveVm;

public partial class SaveDialogViewModel {
    public int uiObservedScore = 0;
    public int uiHealthEvents = 0;
    public int uiDamageTotal = 0;
    public string uiLastStatus = "";
    public int uiCanExecNotifyCount = 0;

    public void BindGeneratedUi() {
        this.SubscribeHealthChanged((int hp, string status) => {
            this.uiHealthEvents = this.uiHealthEvents + hp;
            this.uiLastStatus = status;
        });
        this.HealthChangedEvent += (int hp, string status) => {
            this.uiHealthEvents = this.uiHealthEvents + 1;
        };
        this.SubscribeOnDamage((int dmg) => {
            this.uiDamageTotal = this.uiDamageTotal + dmg;
        });
        this.AttackCommand.SubscribeCanExecuteChanged(() => {
            this.uiCanExecNotifyCount = this.uiCanExecNotifyCount + 1;
        });
    }

    public void TriggerUiCommands(int power) {
        this.AttackCommand.Execute(power);
        this.ResetCommand.Execute();
        this.PingCommand.Execute();
    }
}
)RS"},
            {"SaveDialogViewModel.rs", R"RS(
module Demo.ReactiveVm;

public partial class SaveDialogViewModel {
    [Observable]
    private int _score = 10;

    public bool attackEnabled = false;
    public bool readyFlag = false;
    public int attackTotal = 0;
    public int resetCount = 0;
    public int pingCount = 0;

    public bool IsReady {
        get { return this.readyFlag; }
    }

    [Signal]
    public partial void HealthChanged(int hp, string status);

    [Signal]
    partial void OnDamage(int dmg);

    public void ApplyHit(int dmg) {
        this.OnDamage(dmg);
    }

    [RelayCommand(CanExecute = "CanAttack")]
    public void Attack(int power) {
        this.attackTotal = this.attackTotal + power;
    }

    private bool CanAttack(int power) {
        return this.attackEnabled && power > 0;
    }

    [RelayCommand(CanExecute = "IsReady")]
    public void Reset() {
        this.resetCount = this.resetCount + 1;
    }

    [RelayCommand]
    public void Ping() {
        this.pingCount = this.pingCount + 1;
    }
}

public int RunThreeWayPartial() {
    var vm = new SaveDialogViewModel();
    vm.BindGeneratedUi();

    // [Observable] coexists cleanly
    vm.Score = vm.Score + 5;
    if (vm.Score != 15) return -1;

    // CanExecute false blocks Attack and Reset, while Ping (unconditional) executes
    vm.TriggerUiCommands(25);
    if (vm.AttackCommand.CanExecute(25) || vm.ResetCommand.CanExecute()) return -2;
    if (!vm.PingCommand.CanExecute()) return -3;
    if (vm.attackTotal != 0 || vm.resetCount != 0 || vm.pingCount != 1) return -4;

    // Flip CanExecute conditions & raise CanExecuteChanged notification
    vm.attackEnabled = true;
    vm.readyFlag = true;
    vm.NotifyAttackCanExecuteChanged();
    if (vm.uiCanExecNotifyCount != 1) return -5;
    if (!vm.AttackCommand.CanExecute(25) || vm.AttackCommand.CanExecute(0)) return -6;

    // Trigger via command object and direct ViewModel hook
    vm.TriggerUiCommands(25);
    vm.ExecuteAttack(5);
    if (vm.attackTotal != 30 || vm.resetCount != 1 || vm.pingCount != 2) return -7;

    // Fire [Signal] public & private signals, then verify Unsubscribe
    SaveDialogViewModel__HealthChangedSignalHandler extraListener = (int hp, string status) => {
        vm.uiObservedScore = vm.uiObservedScore + hp;
    };
    vm.SubscribeHealthChanged(extraListener);
    vm.HealthChanged(50, "ok");
    if (vm.uiHealthEvents != 51 || vm.uiObservedScore != 50 || vm.uiLastStatus != "ok") return -8;

    vm.UnsubscribeHealthChanged(extraListener);
    vm.HealthChanged(10, "fine");
    if (vm.uiObservedScore != 50 || vm.uiHealthEvents != 62 || vm.uiLastStatus != "fine") return -9;

    vm.ApplyHit(17);
    if (vm.uiDamageTotal != 17) return -10;

    return vm.Score + vm.attackTotal + vm.uiHealthEvents + vm.uiDamageTotal;
}
)RS"}
        }, std::move(opts));

        runtime::Interpreter interp(prog);
        const auto res = interp.invoke("Demo.ReactiveVm::RunThreeWayPartial");
        // Expected: 15 (Score) + 30 (attackTotal) + 62 (uiHealthEvents) + 17 (uiDamageTotal) = 124
        require(
            res.succeeded && std::holds_alternative<std::int64_t>(res.value) &&
                std::get<std::int64_t>(res.value) == 124,
            "RunThreeWayPartial failed (val=" +
                (res.succeeded && std::holds_alternative<std::int64_t>(res.value)
                    ? std::to_string(std::get<std::int64_t>(res.value))
                    : res.error.message) +
                ")");
    }

    // B. partial struct with [Signal] (delegate-backed, avoiding RS8315) & [RelayCommand]
    {
        compiler::CompilationOptions opts;
        opts.macroPlugins.push_back(compiler::createStandardReactiveMacroPlugin());
        auto prog = buildAndLink({
            {"CounterStruct.rscript", R"RS(
module Demo.ReactiveStruct;

public class StructLog {
    public int sum = 0;
}

public partial struct StepCounter {
    public int count;
    public bool allowStep;

    [Signal]
    public partial void Stepped(int stepCount);

    [RelayCommand(CanExecute = "CanStep")]
    public void Step() {
        this.count = this.count + 1;
        this.Stepped(this.count);
    }

    private bool CanStep() {
        return this.allowStep;
    }
}

public int RunStructReactive() {
    var log = new StructLog();
    StepCounter s = new StepCounter();
    s.count = 0;
    s.allowStep = false;
    s.SubscribeStepped((int c) => {
        log.sum = log.sum + c;
    });
    s.ExecuteStep();
    if (s.count != 0 || log.sum != 0) return -1;
    s.allowStep = true;
    s.ExecuteStep();
    s.ExecuteStep();
    return s.count * 10 + log.sum;
}
)RS"}
        }, std::move(opts));

        runtime::Interpreter interp(prog);
        const auto res = interp.invoke("Demo.ReactiveStruct::RunStructReactive");
        require(
            res.succeeded && std::holds_alternative<std::int64_t>(res.value) &&
                std::get<std::int64_t>(res.value) == 23,
            "RunStructReactive failed: " + res.error.message);
    }

    // C. Error diagnostics for invalid [Signal] & [RelayCommand] usage
    {
        compiler::CompilationOptions opts;
        opts.macroPlugins.push_back(compiler::createStandardReactiveMacroPlugin());

        compiler::Compilation badSignal({{"BadSignal.rscript", R"RS(
module Demo.BadSignal;
public class NotPartial {
    [Signal]
    public partial void Ping();
}
public partial class NonPartialMethod {
    [Signal]
    public void Ping() {}
    [Signal]
    public partial int BadRet();
    [Signal]
    public partial void HasBody() {}
}
)RS"}}, opts);
        const auto dSig = badSignal.build().diagnostics;
        require(hasDiagnosticCode(dSig, "RS9502"), "expected RS9502 on non-partial class");
        require(hasDiagnosticCode(dSig, "RS9503"), "expected RS9503 on non-partial method");
        require(hasDiagnosticCode(dSig, "RS9504"), "expected RS9504 on non-void [Signal]");
        require(hasDiagnosticCode(dSig, "RS9505"), "expected RS9505 on [Signal] with body");

        compiler::Compilation badCmd({{"BadCmd.rscript", R"RS(
module Demo.BadCmd;
public partial class BadVm {
    [RelayCommand]
    public void TooMany(int a, int b) {}
}
)RS"}}, opts);
        require(
            hasDiagnosticCode(badCmd.build().diagnostics, "RS9513"),
            "expected RS9513 on 2-arg [RelayCommand]");
    }
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto fn) {
        try {
            fn();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        }
    };

    run("SourceText #line / #line hidden / #line default / BOM / overflow", testSourceTextLineDirectivesAndBom);
    run("Literals, trivia, control flow & definite assignment", testLiteralsTriviaControlFlowAndDefiniteAssignment);
    run("OOP, default interface methods, generics & pattern matching", testOopInterfacesGenericsAndPatterns);
    run("Speculative lambda overload resolution & ambiguity", testSpeculativeLambdaOverloads);
    run("Exact numerics & [ValueLifecycle] using protocol", testExactNumericsAndValueLifecycle);
    run("[StackOnly] Span<T>/ReadOnlySpan<T> traps, privacy & RS2692..RS2697", testStackOnlySpanAndAllEscapeGuards);
    run("ModulePackRegistry, CompilerSession, ICompilerMacroPlugin & partial diagnostics", testPacksSessionMacrosAndPartials);
    run("[Blittable] layout, [FieldTag]/[FormerlySerializedAs] .rsbc, ProjectConfig & C11 macro cache", testBlittableSchemaEvolutionProjectConfigAndMacroCache);
    run("C# base syntax primitives (verbatim, =>, :this, iface:iface, arity generics, lambdas)", testCSharpBaseSyntaxPrimitives);
    run("Standard Reactive Macros ([Signal] & [RelayCommand] 3-way partial)", testStandardReactiveMacros);

    return failures == 0 ? 0 : 1;
}
