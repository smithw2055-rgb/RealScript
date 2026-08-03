#include "realscript/syntax/Syntax.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/jit/Jit.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/text/Text.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool has(const std::vector<realscript::syntax::SyntaxToken>& values,
         realscript::syntax::SyntaxKind kind) {
    return std::any_of(values.begin(), values.end(),
        [&](const auto& value) { return value.kind == kind; });
}
void testSyntax() {
    const char* source = R"(
module Phase19.Syntax;
public interface IEntity { public int Kind(); }
public abstract class Unit : IEntity
{
    protected int health;
    protected Unit(int initial) : base() { health = initial; }
    public virtual int Power() { return health; }
    public abstract int Kind();
}
internal sealed class Marine : Unit, IEntity
{
    public Marine(int initial) : base(initial) { }
    public sealed override int Power() { return base.Power() + 2; }
    public override int Kind() { return 1; }
}
)";
    realscript::text::SourceText text(source, "phase19.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(text, diagnostics);
    auto unit = parser.parseCompilationUnit();
    require(!diagnostics.hasErrors(), "Phase 19 parser produced diagnostics");
    require(unit.classes.size() == 2 && unit.interfaces.size() == 1,
        "Phase 19 declarations were not parsed");
    const auto& base = unit.classes.front();
    require(has(base.modifiers, realscript::syntax::SyntaxKind::PublicKeyword) &&
            has(base.modifiers, realscript::syntax::SyntaxKind::AbstractKeyword),
        "class modifiers were lost");
    require(base.interfaces.size() == 1 &&
            base.fields.size() == 1 &&
            has(base.fields.front().modifiers,
                realscript::syntax::SyntaxKind::ProtectedKeyword),
        "base list or field modifiers were lost");
    require(base.constructors.front().baseKeyword.has_value() &&
            base.methods.back().semicolonToken.has_value() &&
            has(base.methods.back().modifiers,
                realscript::syntax::SyntaxKind::AbstractKeyword),
        "base initializer or abstract declaration was lost");
    const auto& derived = unit.classes.back();
    require(derived.interfaces.size() == 2 &&
            derived.constructors.front().baseArguments.size() == 1 &&
            has(derived.methods.front().modifiers,
                realscript::syntax::SyntaxKind::OverrideKeyword),
        "derived syntax was not retained");
    const auto& returned = static_cast<const realscript::syntax::ReturnStatementSyntax&>(
        *derived.methods.front().body.statements.front());
    const auto& binary = static_cast<const realscript::syntax::BinaryExpressionSyntax&>(
        *returned.expression);
    const auto& call = static_cast<const realscript::syntax::MemberCallExpressionSyntax&>(
        *binary.left);
    require(call.receiver->kind() == realscript::syntax::SyntaxKind::BaseExpression,
        "base expression did not survive parsing");
}
void testDuplicateModifier() {
    realscript::text::SourceText text(
        "public public class Bad {}", "bad.rs");
    realscript::diagnostics::DiagnosticBag diagnostics;
    realscript::syntax::Parser parser(text, diagnostics);
    (void)parser.parseCompilationUnit();
    require(std::any_of(diagnostics.items().begin(), diagnostics.items().end(),
        [](const auto& value) { return value.code == "RS1116"; }),
        "duplicate modifier did not report RS1116");
}
void testSingleInheritanceExecution() {
    const char* source = R"(
module Phase19.Runtime;
class Unit
{
    int health;
    int Read() { return health; }
}
class Marine : Unit
{
    void Set(int value) { health = value; }
    int ReadBase() { return base.Read(); }
}
int main()
{
    Marine marine = new Marine();
    marine.Set(7);
    return marine.Read() + marine.ReadBase();
}
)";
    realscript::compiler::Compilation compilation({{"runtime.rs", source}});
    auto build = compilation.build();
    if (build.diagnostics.hasErrors()) {
        std::string messages;
        for (const auto& diagnostic : build.diagnostics.items()) {
            if (!messages.empty()) messages.push_back('\n');
            messages += diagnostic.code + ": " + diagnostic.message;
        }
        throw std::runtime_error(
            "single inheritance source failed to compile:\n" + messages);
    }
    require(build.modules.size() == 1,
        "single inheritance did not produce one module");
    const auto marineId = realscript::semantic::stableTypeId(
        "Phase19.Runtime::Marine");
    const auto unitId = realscript::semantic::stableTypeId(
        "Phase19.Runtime::Unit");
    const realscript::semantic::TypeSymbol* marine = nullptr;
    for (const auto& type : build.modules.front().types) {
        if (type.id == marineId) marine = &type;
    }
    require(marine && marine->baseTypeId == unitId &&
            marine->fields.size() == 1 &&
            marine->fields.front().index == 0,
        "base-first class layout was not emitted");
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    modules.push_back(lowerer.lower(build.modules.front()));
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase19.Runtime::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 14,
        "single inheritance execution returned the wrong result: " +
            result.error.message);
}

std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics.items()) {
        if (!result.empty()) result.push_back('\n');
        result += diagnostic.code + ": " + diagnostic.message;
    }
    return result;
}

bool hasDiagnostic(
    const realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& code) {
    return std::any_of(
        diagnostics.items().begin(), diagnostics.items().end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}

void testVirtualDispatchExecution() {
    const char* source = R"(
module Phase19.Virtual;
abstract class Unit
{
    public abstract int Kind();
    public virtual int Power() { return 1; }
}
class Marine : Unit
{
    public override int Kind() { return 7; }
    public override int Power() { return base.Power() + 2; }
}
int Read(Unit value)
{
    return value.Kind() + value.Power();
}
int main()
{
    Unit value = new Marine();
    return Read(value);
}
)";
    realscript::compiler::Compilation compilation({{"virtual.rs", source}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "virtual dispatch source failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    require(build.modules.size() == 1,
        "virtual dispatch did not produce one module");

    const auto unitId = realscript::semantic::stableTypeId(
        "Phase19.Virtual::Unit");
    const auto marineId = realscript::semantic::stableTypeId(
        "Phase19.Virtual::Marine");
    const realscript::semantic::TypeSymbol* unit = nullptr;
    const realscript::semantic::TypeSymbol* marine = nullptr;
    for (const auto& type : build.modules.front().types) {
        if (type.id == unitId) unit = &type;
        if (type.id == marineId) marine = &type;
    }
    require(unit && marine && unit->virtualDispatchTable.size() == 2 &&
            marine->virtualDispatchTable.size() == 2 &&
            unit->virtualDispatchTable[0] == 0 &&
            marine->virtualDispatchTable[0] != 0 &&
            marine->virtualDispatchTable[1] !=
                unit->virtualDispatchTable[1],
        "stable virtual slots were not materialized");

    bool sawVirtualKind = false;
    bool sawVirtualPower = false;
    bool sawStaticBasePower = false;
    for (const auto& function : build.modules.front().functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.opcode != realscript::mir::Opcode::Call) {
                    continue;
                }
                if (instruction.virtualDispatch &&
                    instruction.virtualSlot == 0) {
                    sawVirtualKind = true;
                }
                if (instruction.virtualDispatch &&
                    instruction.virtualSlot == 1) {
                    sawVirtualPower = true;
                }
                if (!instruction.virtualDispatch &&
                    instruction.symbolName.find("Unit.Power") !=
                        std::string::npos) {
                    sawStaticBasePower = true;
                }
            }
        }
    }
    require(sawVirtualKind && sawVirtualPower && sawStaticBasePower,
        "virtual or base-static MIR call metadata is incomplete");

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    modules.push_back(lowerer.lower(build.modules.front()));
    bool bytecodeVirtual = false;
    for (const auto& reference : modules.front().functionReferences) {
        bytecodeVirtual = bytecodeVirtual || reference.virtualDispatch;
    }
    require(bytecodeVirtual,
        "virtual dispatch metadata did not reach bytecode references");
    realscript::runtime::Interpreter interpreter(modules);
    const auto interpreted = interpreter.invoke("Phase19.Virtual::main");
    require(interpreted.succeeded &&
            std::get<std::int64_t>(interpreted.value) == 10,
        "interpreter virtual dispatch returned the wrong result: " +
            interpreted.error.message);

    realscript::diagnostics::DiagnosticBag aotDiagnostics;
    realscript::aot::CppGenerator generator;
    realscript::aot::GenerationOptions options;
    options.programName = "Phase19Virtual";
    const auto generated = generator.generate(
        build.modules, aotDiagnostics, options);
    require(!aotDiagnostics.hasErrors() &&
            generated.source.find("_virtualSlots") != std::string::npos &&
            generated.source.find("CallSignature") != std::string::npos &&
            generated.source.find("true,") != std::string::npos,
        "AOT output did not retain virtual dispatch metadata");

#if defined(REALSCRIPT_PHASE19_JIT_COMPILER)
    realscript::jit::ToolchainOptions jitOptions;
    jitOptions.compiler = REALSCRIPT_PHASE19_JIT_COMPILER;
    jitOptions.includeDirectory = REALSCRIPT_PHASE19_JIT_INCLUDE_DIR;
    jitOptions.supportLibrary =
        REALSCRIPT_PHASE19_JIT_SUPPORT_LIBRARY;
    jitOptions.outputDirectory = REALSCRIPT_PHASE19_JIT_CACHE_DIR;
    jitOptions.generation.programName = "Phase19VirtualJit";
    realscript::jit::ToolchainJit jit;
    auto compiled = jit.compile(build.modules, jitOptions);
    require(compiled.succeeded(),
        compiled.error.empty()
            ? "Phase 19 virtual JIT compilation failed"
            : compiled.error);
    const auto jitResult = compiled.module->invoke(
        "Phase19.Virtual::main");
    require(jitResult.succeeded &&
            std::get<std::int64_t>(jitResult.value) == 10,
        "JIT virtual dispatch returned the wrong result: " +
            jitResult.error.message);
#endif
}

void testVirtualDiagnostics() {
    const auto compile = [](const char* source) {
        realscript::compiler::Compilation compilation({{"invalid.rs", source}});
        return compilation.build();
    };

    auto missing = compile(R"(
abstract class Base { public abstract int Run(); }
class Bad : Base { }
)");
    require(hasDiagnostic(missing.diagnostics, "RS2523"),
        "concrete class with an abstract slot did not report RS2523");

    auto nonVirtual = compile(R"(
class Base { public int Run() { return 1; } }
class Bad : Base { public override int Run() { return 2; } }
)");
    require(hasDiagnostic(nonVirtual.diagnostics, "RS2519"),
        "override of a non-virtual method did not report RS2519");

    auto sealed = compile(R"(
class Base { public virtual int Run() { return 1; } }
class Middle : Base { public sealed override int Run() { return 2; } }
class Bad : Middle { public override int Run() { return 3; } }
)");
    require(hasDiagnostic(sealed.diagnostics, "RS2520"),
        "override of a sealed method did not report RS2520");

    auto allocation = compile(R"(
abstract class Base { public abstract int Run(); }
int main() { Base value = new Base(); return 0; }
)");
    require(hasDiagnostic(allocation.diagnostics, "RS2524"),
        "abstract class allocation did not report RS2524");
}

}
int main() {
    try {
        testSyntax();
        testDuplicateModifier();
        testSingleInheritanceExecution();
        testVirtualDispatchExecution();
        testVirtualDiagnostics();
        std::cout << "Phase 19 runtime polymorphism tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
