#include "realscript/syntax/Syntax.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/jit/Jit.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/game/GameBytecodeObjects.h"
#include "realscript/tooling/LanguageService.h"
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
std::string diagnosticsText(
    const realscript::diagnostics::DiagnosticBag& diagnostics);
bool hasDiagnostic(
    const realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& code);
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

void testBaseConstructorExecution() {
    const char* source = R"(
module Phase19.Constructors;
class Base
{
    protected int value;
    protected Base(int initial) { value = initial; }
    public int Read() { return value; }
}
class Derived : Base
{
    public Derived(int initial) : base(initial + 1)
    {
        value = value + 1;
    }
}
int main()
{
    Derived value = new Derived(40);
    return value.Read();
}
)";
    realscript::compiler::Compilation compilation(
        {{"base-constructor.rs", source}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "base constructor source failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    modules.push_back(lowerer.lower(build.modules.front()));
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase19.Constructors::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "base constructor did not execute before the derived body: " +
            result.error.message);

    realscript::compiler::Compilation invalid({{"invalid-base.rs", R"(
class Base { protected Base(int initial) { } }
class Derived : Base { public Derived() { } }
)"}});
    const auto invalidBuild = invalid.build();
    require(hasDiagnostic(invalidBuild.diagnostics, "RS2532"),
        "missing implicit base constructor did not report RS2532");
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

void testVisibilityDiagnostics() {
    realscript::compiler::Compilation privateMember({{"private-member.rs", R"(
class Box
{
    private int Secret() { return 7; }
    public int Reveal() { return Secret(); }
}
int main()
{
    Box value = new Box();
    return value.Secret();
}
)"}});
    const auto privateBuild = privateMember.build();
    require(hasDiagnostic(privateBuild.diagnostics, "RS2472"),
        "private method access was not rejected");

    realscript::compiler::Compilation privateConstructor(
        {{"private-constructor.rs", R"(
class Locked { private Locked() { } }
int main() { Locked value = new Locked(); return 0; }
)"}});
    const auto constructorBuild = privateConstructor.build();
    require(hasDiagnostic(constructorBuild.diagnostics, "RS2474"),
        "private constructor access was not rejected");

    realscript::compiler::Compilation protectedMember(
        {{"protected-member.rs", R"(
class Base { protected int value; }
class Other
{
    public int Read(Base input) { return input.value; }
}
)"}});
    const auto protectedBuild = protectedMember.build();
    require(hasDiagnostic(protectedBuild.diagnostics, "RS2534"),
        "protected field access from an unrelated type was not rejected");

    realscript::compiler::Compilation internalType({
        {"hidden.rs", R"(
module Hidden;
internal class HiddenType { public HiddenType() { } }
)"},
        {"consumer.rs", R"(
module Consumer;
import Hidden;
int main() { HiddenType value = new HiddenType(); return 0; }
)"},
    });
    const auto internalBuild = internalType.build();
    require(hasDiagnostic(internalBuild.diagnostics, "RS2534"),
        "internal type access across modules was not rejected");
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

void testInterfaceDispatchExecution() {
    const char* source = R"(
module Phase19.Interfaces;
public interface IUnit
{
    public int Power();
    public int Kind();
}
public class Marine : Unit
{
    public override int Power() { return 7; }
}
public class Unit : IUnit
{
    public virtual int Power() { return 1; }
    public int Kind() { return 3; }
}
IUnit Identity(IUnit value)
{
    return value;
}
int Read(IUnit value)
{
    return value.Power() + value.Kind();
}
int main()
{
    IUnit value = new Marine();
    IUnit[] values = new IUnit[1];
    values[0] = value;
    return Read(Identity(values[0]));
}
)";

    realscript::compiler::Compilation compilation(
        {{"interfaces.rs", source}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "interface dispatch source failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    require(build.modules.size() == 1,
        "interface dispatch did not produce one module");

    const auto interfaceId = realscript::semantic::stableTypeId(
        "Phase19.Interfaces::IUnit");
    const auto marineId = realscript::semantic::stableTypeId(
        "Phase19.Interfaces::Marine");
    const realscript::semantic::TypeSymbol* interfaceType = nullptr;
    const realscript::semantic::TypeSymbol* marine = nullptr;
    for (const auto& type : build.modules.front().types) {
        if (type.id == interfaceId) interfaceType = &type;
        if (type.id == marineId) marine = &type;
    }
    require(interfaceType && interfaceType->interfaceType &&
            interfaceType->methods.size() == 2,
        "interface type descriptor was not materialized");
    const auto kind = std::find_if(
        interfaceType->methods.begin(), interfaceType->methods.end(),
        [](const auto& method) { return method.name == "Kind"; });
    const auto power = std::find_if(
        interfaceType->methods.begin(), interfaceType->methods.end(),
        [](const auto& method) { return method.name == "Power"; });
    require(kind != interfaceType->methods.end() &&
            power != interfaceType->methods.end() &&
            kind->interfaceSlot == 0 &&
            power->interfaceSlot == 1,
        "interface slots are not stable by canonical signature");

    const realscript::semantic::InterfaceDispatchMap*
        implementation = nullptr;
    if (marine) {
        const auto found = std::find_if(
            marine->interfaceDispatchMaps.begin(),
            marine->interfaceDispatchMaps.end(),
            [&](const auto& map) {
                return map.interfaceTypeId == interfaceId;
            });
        if (found != marine->interfaceDispatchMaps.end()) {
            implementation = &*found;
        }
    }
    require(marine && implementation &&
            implementation->slots.size() == 2 &&
            implementation->slots[0] != 0 &&
            implementation->slots[1] != 0,
        "derived class interface dispatch map is incomplete");

    bool sawKind = false;
    bool sawPower = false;
    for (const auto& function : build.modules.front().functions) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (!instruction.interfaceDispatch) continue;
                require(instruction.interfaceTypeId == interfaceId,
                    "interface MIR call has the wrong interface type");
                sawKind = sawKind || instruction.interfaceSlot == 0;
                sawPower = sawPower || instruction.interfaceSlot == 1;
            }
        }
    }
    require(sawKind && sawPower,
        "interface dispatch metadata did not reach MIR calls");

    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    modules.push_back(lowerer.lower(build.modules.front()));
    const bool bytecodeInterface = std::any_of(
        modules.front().functionReferences.begin(),
        modules.front().functionReferences.end(),
        [](const auto& reference) {
            return reference.interfaceDispatch;
        });
    require(bytecodeInterface,
        "interface dispatch metadata did not reach bytecode references");

    realscript::runtime::Interpreter interpreter(modules);
    const auto interpreted = interpreter.invoke(
        "Phase19.Interfaces::main");
    require(interpreted.succeeded &&
            std::get<std::int64_t>(interpreted.value) == 10,
        "interpreter interface dispatch returned the wrong result: " +
            interpreted.error.message);

    realscript::diagnostics::DiagnosticBag aotDiagnostics;
    realscript::aot::CppGenerator generator;
    realscript::aot::GenerationOptions options;
    options.programName = "Phase19Interfaces";
    const auto generated = generator.generate(
        build.modules, aotDiagnostics, options);
    require(!aotDiagnostics.hasErrors() &&
            generated.source.find("_interfaceMaps") !=
                std::string::npos &&
            generated.source.find("CallSignature") !=
                std::string::npos,
        "AOT output did not retain interface dispatch metadata");

#if defined(REALSCRIPT_PHASE19_JIT_COMPILER)
    realscript::jit::ToolchainOptions jitOptions;
    jitOptions.compiler = REALSCRIPT_PHASE19_JIT_COMPILER;
    jitOptions.includeDirectory = REALSCRIPT_PHASE19_JIT_INCLUDE_DIR;
    jitOptions.supportLibrary =
        REALSCRIPT_PHASE19_JIT_SUPPORT_LIBRARY;
    jitOptions.outputDirectory = REALSCRIPT_PHASE19_JIT_CACHE_DIR;
    jitOptions.generation.programName = "Phase19InterfacesJit";
    realscript::jit::ToolchainJit jit;
    auto compiled = jit.compile(build.modules, jitOptions);
    require(compiled.succeeded(),
        compiled.error.empty()
            ? "Phase 19 interface JIT compilation failed"
            : compiled.error);
    const auto jitResult = compiled.module->invoke(
        "Phase19.Interfaces::main");
    require(jitResult.succeeded &&
            std::get<std::int64_t>(jitResult.value) == 10,
        "JIT interface dispatch returned the wrong result: " +
            jitResult.error.message);
#endif
}

void testObjectModelBytecodeRoundTrip() {
    const char* source = R"(
module Phase19.Rsbc;
public interface IUnit { public int Read(); }
public abstract class Unit : IUnit
{
    public abstract int Read();
}
public sealed class Marine : Unit
{
    private int stored;
    public Marine() { stored = 19; }
    public int Value { get { return stored; } }
    public override int Read() { return Value; }
}
int main()
{
    IUnit value = new Marine();
    return value.Read();
}
)";
    realscript::compiler::Compilation compilation({{"phase19-rsbc.rs", source}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 19 RSBC source failed to compile:\n" +
            diagnosticsText(build.diagnostics));

    realscript::bytecode::Lowerer lowerer;
    const auto module = lowerer.lower(build.modules.front());
    require(module.version.major == 0 && module.version.minor == 7,
        "Phase 19 object model did not advance RSBC to 0.7");
    const auto encoded = realscript::bytecode::encodeModule(module);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(realscript::bytecode::decodeModule(
            encoded, decoded, diagnostics),
        "Phase 19 RSBC failed to decode:\n" + diagnosticsText(diagnostics));
    require(realscript::bytecode::encodeModule(decoded) == encoded,
        "Phase 19 RSBC round trip is not canonical");
    const auto verified = realscript::bytecode::verifyModule(
        decoded, diagnostics);
    require(verified,
        "decoded Phase 19 RSBC failed verification:\n" +
            diagnosticsText(diagnostics));

    auto corrupted = encoded;
    const auto readU32 = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(corrupted[offset]) |
            (static_cast<std::uint32_t>(corrupted[offset + 1]) << 8) |
            (static_cast<std::uint32_t>(corrupted[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(corrupted[offset + 3]) << 24);
    };
    const auto objectModelOffset = readU32(16 + 7 * 12 + 4);
    corrupted[objectModelOffset] = 0xffu;
    corrupted[objectModelOffset + 1] = 0xffu;
    corrupted[objectModelOffset + 2] = 0xffu;
    corrupted[objectModelOffset + 3] = 0xffu;
    realscript::bytecode::Module rejected;
    realscript::diagnostics::DiagnosticBag corruptionDiagnostics;
    require(!realscript::bytecode::decodeModule(
            corrupted, rejected, corruptionDiagnostics) &&
            hasDiagnostic(corruptionDiagnostics, "RS5004"),
        "corrupted Phase 19 object-model section was accepted");

    const auto marineId = realscript::semantic::stableTypeId(
        "Phase19.Rsbc::Marine");
    const auto marine = std::find_if(
        decoded.types.begin(), decoded.types.end(),
        [&](const auto& type) { return type.id == marineId; });
    require(marine != decoded.types.end() && marine->sealedType &&
            marine->baseTypeId != 0 &&
            marine->fields.size() == 1 &&
            marine->fields.front().accessibility ==
                realscript::semantic::Accessibility::Private &&
            marine->constructors.size() == 1 &&
            marine->properties.size() == 1 &&
            marine->virtualDispatchTable.size() == 1 &&
            marine->interfaceDispatchMaps.size() == 1,
        "decoded Phase 19 object-model descriptor is incomplete");
    require(std::any_of(
            decoded.functionReferences.begin(),
            decoded.functionReferences.end(),
            [](const auto& reference) {
                return reference.interfaceDispatch &&
                    reference.interfaceTypeId != 0 &&
                    reference.interfaceSlot == 0;
            }),
        "decoded Phase 19 interface-call metadata is incomplete");

    std::vector<realscript::bytecode::Module> modules;
    modules.push_back(std::move(decoded));
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase19.Rsbc::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 19,
        "decoded Phase 19 RSBC dispatch returned the wrong result: " +
            result.error.message);
}

void testGameSdkInheritedObjectClosure() {
    realscript::game::GameApi api;
    realscript::game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"phase19-game-sdk.rs", R"(
module Phase19.GameSdk;
class Base
{
    protected int baseValue;
    protected Base(int value) { baseValue = value; }
}
class Derived : Base
{
    private int ownValue;
    public Derived() : base(19) { ownValue = 23; }
    public int Total { get { return baseValue + ownValue; } }
    public int Read() { return Total; }
}
)"}});
    require(compiled.succeeded(),
        "Phase 19 Game SDK source failed to compile:\n" +
            diagnosticsText(compiled.diagnostics));
    std::vector<std::vector<std::uint8_t>> encoded;
    for (const auto& module : compiled.modules) {
        encoded.push_back(realscript::bytecode::encodeModule(module));
    }
    auto loaded = realscript::game::loadGameObjectBytecodeModules(
        api, encoded);
    require(loaded.succeeded(),
        "Phase 19 Game SDK bytecode failed to load:\n" +
            diagnosticsText(loaded.diagnostics));
    auto runtime = loaded.package.createRuntime();
    const auto type = runtime.findType("Phase19.GameSdk::Derived");
    require(type.has_value() && type->descriptor.baseTypeId != 0 &&
            type->descriptor.fields.size() == 2 &&
            type->descriptor.constructors.size() == 1 &&
            type->descriptor.properties.size() == 1,
        "loaded Game SDK type lost inherited object metadata");
    const auto method = runtime.findMethod(*type, "Read", 0);
    require(method.has_value(),
        "loaded Game SDK type lost its public method contract");
    realscript::runtime::RuntimeError error;
    auto object = runtime.createObject(*type, {}, error);
    require(object.has_value(),
        "loaded Game SDK object construction failed: " + error.message);
    const auto invoked = runtime.invoke(*object, *method);
    require(invoked.succeeded &&
            std::get<std::int64_t>(invoked.value) == 42,
        "loaded Game SDK inherited object returned the wrong result: " +
            invoked.error.message);
    const auto snapshot = realscript::game::snapshotScriptObject(
        runtime, *object, error);
    require(snapshot.has_value() && snapshot->fields.size() == 2,
        "inherited script-object state did not retain both fields: " +
            error.message);
}

void testLanguageToolingVisibilityAndInheritance() {
    realscript::tooling::LanguageService service;
    service.open("base-tools.rs", R"(
module Phase19.Tools;
class Base
{
    private int Secret() { return 1; }
    protected int Shared() { return 2; }
    public int Visible() { return 3; }
}
)" );
    service.open("derived-tools.rs", R"(
module Phase19.Tools;
class Derived : Base
{
    int Run() { return Shared(); }
}
)" );
    require(service.diagnostics("derived-tools.rs").empty(),
        "inherited tooling fixture produced diagnostics");
    const auto completion = service.completion(
        "derived-tools.rs", {4, 24});
    require(std::any_of(
            completion.begin(), completion.end(),
            [](const auto& item) { return item.label == "Shared"; }) &&
            std::none_of(
                completion.begin(), completion.end(),
                [](const auto& item) { return item.label == "Secret"; }),
        "LSP completion did not respect inherited visibility");
    const auto definition = service.definition(
        "derived-tools.rs", {4, 25});
    require(definition && definition->path == "base-tools.rs",
        "LSP definition did not resolve the inherited method");
    const auto edits = service.rename(
        "derived-tools.rs", {4, 25}, "SharedValue");
    require(edits.size() == 2,
        "LSP rename did not include the inherited definition and call");

    service.open("internal-tools.rs", R"(
module Phase19.HiddenTools;
internal class HiddenType { }
)" );
    service.open("consumer-tools.rs", R"(
module Phase19.ConsumerTools;
import Phase19.HiddenTools;
int main() { return 0; }
)" );
    const auto externalCompletion = service.completion(
        "consumer-tools.rs", {3, 12});
    require(std::none_of(
            externalCompletion.begin(), externalCompletion.end(),
            [](const auto& item) { return item.label == "HiddenType"; }),
        "LSP completion exposed an internal type across modules");
}

void testDebuggerShowsRuntimeUserMethod() {
    realscript::compiler::Compilation compilation({{"phase19-debug.rs", R"(
module Phase19.Debug;
interface IValue { public int Read(); }
abstract class Base : IValue { public abstract int Read(); }
class Derived : Base
{
    public override int Read()
    {
        return 19;
    }
}
int main()
{
    IValue value = new Derived();
    return value.Read();
}
)"}});
    auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 19 debugger source failed to compile:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    bool sawDerivedUserMethod = false;
    for (const auto& mir : build.modules) {
        const auto module = lowerer.lower(mir);
        for (const auto& function : module.functions) {
            require(function.name.find("$dispatch") == std::string::npos,
                "bytecode exposed a polymorphism dispatch helper as a function");
            if (function.name.find("Derived.Read") != std::string::npos) {
                sawDerivedUserMethod =
                    function.debugInfo.sourceName == "phase19-debug.rs" &&
                    !function.debugInfo.sequencePoints.empty();
            }
        }
    }
    require(sawDerivedUserMethod,
        "runtime override did not retain user-source debugger metadata");
}

void testInterfaceDiagnostics() {
    const auto compile = [](const char* source) {
        realscript::compiler::Compilation compilation(
            {{"invalid-interface.rs", source}});
        return compilation.build();
    };

    auto allocation = compile(R"(
interface IUnit { public int Kind(); }
int main() { IUnit value = new IUnit(); return 0; }
)");
    require(hasDiagnostic(allocation.diagnostics, "RS2530"),
        "interface allocation did not report RS2530");

    auto conversion = compile(R"(
interface IUnit { public int Kind(); }
class Other { public int Kind() { return 1; } }
int main() { IUnit value = new Other(); return value.Kind(); }
)");
    require(conversion.diagnostics.hasErrors(),
        "class without an interface map converted to the interface type");
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
        testBaseConstructorExecution();
        testVisibilityDiagnostics();
        testVirtualDispatchExecution();
        testInterfaceDispatchExecution();
        testObjectModelBytecodeRoundTrip();
        testGameSdkInheritedObjectClosure();
        testLanguageToolingVisibilityAndInheritance();
        testDebuggerShowsRuntimeUserMethod();
        testInterfaceDiagnostics();
        testVirtualDiagnostics();
        std::cout << "Phase 19 runtime polymorphism tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
