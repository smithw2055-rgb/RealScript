#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        result += diagnostic.code + ": " + diagnostic.message + "\n";
    }
    return result;
}

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture: " + path);
    std::ostringstream content;
    content << stream.rdbuf();
    return content.str();
}

void testGrowableCollectionsAndEnumeratorProtocol() {
    const auto path = std::string(REALSCRIPT_SOURCE_DIR) +
        "/tests/fixtures/phase21_aot.rs";
    realscript::compiler::Compilation compilation({{
        "tests/fixtures/phase21_aot.rs", readFile(path)}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 21 collection compilation failed:\n" +
            diagnosticsText(build.diagnostics));

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& sourceModule : build.modules) {
        auto module = lowerer.lower(sourceModule);
        realscript::diagnostics::DiagnosticBag diagnostics;
        require(realscript::bytecode::verifyModule(module, diagnostics),
            "Phase 21 bytecode verification failed:\n" +
                diagnosticsText(diagnostics));
        modules.push_back(std::move(module));
    }
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase21.Aot::main");
    require(result.succeeded,
        "Phase 21 collection execution failed: " + result.error.message);
    require(std::get<std::int64_t>(result.value) == 242,
        "growable collections or enumerator protocol returned " +
            std::to_string(std::get<std::int64_t>(result.value)) +
            " instead of 242");
}

void testCrossModuleSpecializationCache() {
    realscript::compiler::Compilation compilation({
        {"box.rs", R"(
module Phase21.Box;
class Box<T> { T value; Box(T initial) { value = initial; } T Get() { return value; } }
)"},
        {"left.rs", R"(
module Phase21.Left;
import Phase21.Box;
int Left() { Box<int> value = new Box<int>(3); return value.Get(); }
)"},
        {"right.rs", R"(
module Phase21.Right;
import Phase21.Box;
int Right() { Box<int> value = new Box<int>(4); return value.Get(); }
)"},
    });
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "cross-module specialization failed:\n" +
            diagnosticsText(build.diagnostics));
    std::size_t boxSpecializations = 0;
    for (const auto& value : build.nativeGenericInstantiations) {
        if (value.genericName == "Box" &&
            value.generatedName == "Box__int") ++boxSpecializations;
    }
    require(boxSpecializations == 1,
        "the deterministic specialization cache emitted Box<int> more than once");
}

void testInferenceAndGenericContracts() {
    realscript::compiler::Compilation compilation({{
        "phase21-contracts.rs", R"(
module Phase21.Contracts;

interface IValue<T> { T Get(); }
delegate T Factory<T>();

class IntValue : IValue<int>
{
    int Get() { return 7; }
}

T Identity<T>(T value) { return value; }
int Make() { return 5; }

int main()
{
    IValue<int> value = new IntValue();
    Factory<int> factory = Make;
    int current = value.Get();
    return Identity(current) + factory();
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "generic inference/interface/delegate compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) modules.push_back(lowerer.lower(module));
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase21.Contracts::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 12,
        "generic inference/interface/delegate execution produced the wrong result");
}

void testGenericMemberMethodsAndConstraints() {
    realscript::compiler::Compilation compilation({{
        "phase21-members.rs", R"(
module Phase21.Members;
class Converter
{
    T Echo<T>(T value) where T : struct { return value; }
}
int main()
{
    Converter converter = new Converter();
    int first = converter.Echo(8);
    return first + converter.Echo<int>(4);
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "generic member specialization failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) modules.push_back(lowerer.lower(module));
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase21.Members::main");
    require(result.succeeded && std::get<std::int64_t>(result.value) == 12,
        "generic member specialization produced the wrong result");

    realscript::compiler::Compilation invalid({{
        "phase21-bad-constraint.rs", R"(
module Phase21.BadConstraint;
T ReferenceOnly<T>(T value) where T : class { return value; }
int main() { return ReferenceOnly<int>(1); }
)"}});
    const auto rejected = invalid.build();
    bool foundConstraintDiagnostic = false;
    for (const auto& diagnostic : rejected.diagnostics.items()) {
        foundConstraintDiagnostic = foundConstraintDiagnostic ||
            diagnostic.code == "RS8530";
    }
    require(rejected.diagnostics.hasErrors() && foundConstraintDiagnostic,
        "a generic class constraint accepted an int type argument");
}

bool hasGenericDiag(
    const realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& code) {
    for (const auto& d : diagnostics.items()) {
        if (d.code == code) return true;
    }
    return false;
}

void testWhereConstraintsValidationAndEdgeCases() {
    // 1. Expression-bodied generic with where, naked type constraint (where T : U), Nullable<int> as new()
    realscript::compiler::Compilation goodComp({{
        "phase21-where-valid.rs", R"(
module Phase21.WhereValid;
class BaseItem { public int val = 10; }
class DerivedItem : BaseItem { }
class CtorItem { public int val = 32; public CtorItem() {} }

T Id<T>(T x) where T : class => x;
int PairCheck<T, U>(T first, U second) where T : U => 1;
int MakeNew<T>() where T : new() => 1;

int main()
{
    DerivedItem d = new DerivedItem();
    DerivedItem d2 = Id<DerivedItem>(d);
    BaseItem b = new BaseItem();
    int p = PairCheck<DerivedItem, BaseItem>(d2, b);
    int n = MakeNew<CtorItem>();
    return d2.val + p + n;
}
)"}});
    const auto goodBuild = goodComp.build();
    require(!goodBuild.diagnostics.hasErrors(),
        "valid generic where constraints failed:\n" +
            diagnosticsText(goodBuild.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto lowered = lowerer.lower(goodBuild.modules.front());
    realscript::runtime::Interpreter interp({std::move(lowered)});
    const auto res = interp.invoke("Phase21.WhereValid::main");
    require(res.succeeded && std::get<std::int64_t>(res.value) == 12,
        "valid generic where constraints runtime mismatch");

    // 2. Declaration-site RS8531 (unknown param), RS8532 (duplicate constraint), RS8533 (conflicts/ordering/cycles/invalid types)
    const struct {
        const char* name;
        const char* source;
        const char* code;
    } declCases[] = {
        {"unknown_param",
         "module Phase21.D1; T F<T>(T x) where U : class { return x; } int main() => 0;",
         "RS8531"},
        {"duplicate_constraint",
         "module Phase21.D2; T F<T>(T x) where T : class where T : class { return x; } int main() => 0;",
         "RS8532"},
        {"class_and_struct",
         "module Phase21.D3; T F<T>(T x) where T : class, struct { return x; } int main() => 0;",
         "RS8533"},
        {"struct_and_new",
         "module Phase21.D4; T F<T>(T x) where T : struct, new() { return x; } int main() => 0;",
         "RS8533"},
        {"new_not_last",
         "module Phase21.D5; interface IA { int K(); } T F<T>(T x) where T : new(), IA { return x; } int main() => 0;",
         "RS8533"},
        {"class_not_first",
         "module Phase21.D6; interface IA { int K(); } T F<T>(T x) where T : IA, class { return x; } int main() => 0;",
         "RS8533"},
        {"primitive_constraint",
         "module Phase21.D7; T F<T>(T x) where T : int { return x; } int main() => 0;",
         "RS8533"},
        {"sealed_constraint",
         "module Phase21.D8; sealed class S {} T F<T>(T x) where T : S { return x; } int main() => 0;",
         "RS8533"},
        {"missing_type_constraint",
         "module Phase21.D9; T F<T>(T x) where T : DoesNotExist { return x; } int main() => 0;",
         "RS8533"},
        {"self_cycle",
         "module Phase21.D10; T F<T>(T x) where T : T { return x; } int main() => 0;",
         "RS8533"},
        {"mutual_cycle",
         "module Phase21.D11; T F<T, U>(T x) where T : U where U : T { return x; } int main() => 0;",
         "RS8533"},
    };
    for (const auto& tc : declCases) {
        realscript::compiler::Compilation c({{tc.name, tc.source}});
        const auto b = c.build();
        require(b.diagnostics.hasErrors() && hasGenericDiag(b.diagnostics, tc.code),
            std::string("declaration constraint case '") + tc.name + "' expected " + tc.code);
    }

    // 3. Instantiation-site RS8530 checks: abstract/private ctor vs new(), int? vs struct/class, naked param T : U fail
    const struct {
        const char* name;
        const char* source;
    } instCases[] = {
        {"abstract_new",
         "module Phase21.I1; abstract class A { public A() {} } int Make<T>() where T : new() => 1; int main() => Make<A>();"},
        {"private_ctor_new",
         "module Phase21.I2; class P { private P() {} } int Make<T>() where T : new() => 1; int main() => Make<P>();"},
        {"nullable_fails_struct",
         "module Phase21.I3; int Val<T>(T x) where T : struct => 1; int main() { int? n = 5; return Val<int?>(n); }"},
        {"nullable_fails_class",
         "module Phase21.I4; int Ref<T>(T x) where T : class => 1; int main() { int? n = 5; return Ref<int?>(n); }"},
        {"naked_param_fails",
         "module Phase21.I5; class Base {} class Other {} int Pair<T, U>() where T : U => 1; int main() => Pair<Other, Base>();"},
    };
    for (const auto& tc : instCases) {
        realscript::compiler::Compilation c({{tc.name, tc.source}});
        const auto b = c.build();
        require(b.diagnostics.hasErrors() && hasGenericDiag(b.diagnostics, "RS8530"),
            std::string("instantiation constraint case '") + tc.name + "' expected RS8530");
    }
}

void testGenericTypeAndDelegateArityOverloading() {
    realscript::compiler::Compilation okComp({{
        "phase21-arity-overload.rs", R"(
module Phase21.ArityOverload;
public delegate TResult Func<TResult>();
public delegate TResult Func<T, TResult>(T arg);
public class TupleBox<T> {
    public T First;
    public TupleBox(T a) { First = a; }
}
public class TupleBox<T1, T2> {
    public T1 First;
    public T2 Second;
    public TupleBox(T1 a, T2 b) { First = a; Second = b; }
}
int main() {
    Func<int> f0 = () => 10;
    Func<int, int> f1 = (int x) => x + 5;
    TupleBox<int> b1 = new TupleBox<int>(3);
    TupleBox<int, int> b2 = new TupleBox<int, int>(4, 20);
    return f0() + f1(b1.First) + b2.First + b2.Second;
}
)"}});
    const auto okBuild = okComp.build();
    require(!okBuild.diagnostics.hasErrors(),
        "arity-overloaded generic delegates/types failed:\n" +
            diagnosticsText(okBuild.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto lowered = lowerer.lower(okBuild.modules.front());
    realscript::runtime::Interpreter interp({std::move(lowered)});
    const auto res = interp.invoke("Phase21.ArityOverload::main");
    require(res.succeeded && std::get<std::int64_t>(res.value) == 42,
        "arity-overloaded generic delegates/types runtime mismatch");

    realscript::compiler::Compilation dupComp({{
        "phase21-arity-dup.rs", R"(
module Phase21.ArityDup;
public delegate TResult Func<TResult>();
public delegate R Func<R>();
int main() => 0;
)"}});
    const auto dupBuild = dupComp.build();
    require(dupBuild.diagnostics.hasErrors() && hasGenericDiag(dupBuild.diagnostics, "RS8502"),
        "same-arity generic delegate duplicate should report RS8502");

    realscript::compiler::Compilation mismatchComp({{
        "phase21-arity-mismatch.rs", R"(
module Phase21.ArityMismatch;
public delegate TResult Func<TResult>();
public delegate TResult Func<T, TResult>(T arg);
int main() {
    Func<int, int, int> bad = null;
    return bad == null ? 0 : 1;
}
)"}});
    const auto mismatchBuild = mismatchComp.build();
    require(mismatchBuild.diagnostics.hasErrors() && hasGenericDiag(mismatchBuild.diagnostics, "RS8501"),
        "unmatched arity on overloaded generic delegate should report RS8501");
}

} // namespace

int main() {
    try {
        testGrowableCollectionsAndEnumeratorProtocol();
        testCrossModuleSpecializationCache();
        testInferenceAndGenericContracts();
        testGenericMemberMethodsAndConstraints();
        testWhereConstraintsValidationAndEdgeCases();
        testGenericTypeAndDelegateArityOverloading();
        std::cout << "Phase 21 generics and collections tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
