#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/tooling/LanguageService.h"

#include <algorithm>
#include <iostream>
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

bool hasDiagnostic(
    const realscript::diagnostics::DiagnosticBag& diagnostics,
    const std::string& code) {
    for (const auto& diagnostic : diagnostics.items()) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

void testImplicitLocalTypes() {
    realscript::compiler::Compilation compilation({{"phase24_var.rs", R"(
module Phase24.Core;

class Holder
{
    int value;
    public Holder child;
    public Holder(int initial) { value = initial; }
    public int Read() { return value; }
    public Holder Self() { return this; }
}

class BaseValue { }
class DerivedValue : BaseValue
{
    public int Read() { return 41; }
}
class OtherValue : BaseValue { }

class Settings
{
    int stored;
    public int Value { get { return stored; } set { stored = value; } }
}

struct Pair
{
    public int left;
    public int right;
}

class Recorder
{
    public int stamp;
    public int Next(int value) { stamp = stamp * 10 + value; return value; }
    public int Capture(int first, int second = 4, params int[] rest)
    {
        int total = first + second;
        int index = 0;
        while (index < rest.length) { total = total + rest[index]; index = index + 1; }
        return total;
    }
}

class OptionalBox
{
    int value;
    public OptionalBox(int initial = 6) { value = initial; }
    public int Read() { return value; }
}

class ScriptError { public int code; }
class DerivedError : ScriptError { }
class OtherError : ScriptError { }

class CleanupProbe
{
    public int value;
    public int ReturnFromTry()
    {
        try { return value; }
        finally { value = value + 1; }
    }
}

int InferLocals(int input)
{
    var number = input + 2;
    var enabled = true;
    var text = "ok";
    var values = new int[2];
    var holder = new Holder(number);
    values[0] = holder.Read();
    values[1] = enabled ? 1 : 0;
    return values[0] + values[1] + 2;
}

int ConditionalIsLazy(bool condition, int divisor)
{
    return condition ? 7 : 10 / divisor;
}

Holder ConditionalReference(bool condition)
{
    return condition ? new Holder(11) : null;
}

int CoalesceHolder(Holder input, int divisor)
{
    var selected = input ?? new Holder(10 / divisor);
    return selected.Read();
}

int NullConditionalCall(bool hasValue)
{
    Holder input = hasValue ? new Holder(17) : null;
    var selected = input?.Self() ?? new Holder(9);
    return selected.Read();
}

int NullConditionalField(bool hasValue)
{
    Holder input = hasValue ? new Holder(1) : null;
    if (hasValue) input.child = new Holder(23);
    var selected = input?.child ?? new Holder(6);
    return selected.Read();
}

int NullConditionalValue(bool hasValue)
{
    Holder input = hasValue ? new Holder(29) : null;
    return input?.Read() ?? 31;
}

bool LiftedNullableMembers(bool hasValue)
{
    Holder input = hasValue ? new Holder(29) : null;
    var lifted = input?.Read();
    return hasValue
        ? lifted.HasValue() && lifted.Value() == 29
        : !lifted.HasValue() && lifted.GetValueOrDefault() == 0;
}

bool RuntimeIs(bool derived)
{
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    return value is DerivedValue;
}

int RuntimeAs(bool derived)
{
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    DerivedValue selected = value as DerivedValue;
    return selected == null ? -1 : selected.Read();
}

bool TypeTokens()
{
    return typeof(DerivedValue) == typeof(DerivedValue) &&
        typeof(DerivedValue) != typeof(BaseValue);
}

int Initializers()
{
    var holder = new Holder(1) { child = new Holder(37) };
    var settings = new Settings { Value = 5 };
    var pair = new Pair { left = 2, right = 3 };
    var values = new List<int>() { 7, 11 };
    var lookup = new Dictionary<int, int>() { { 4, 13 } };
    return holder.child.Read() + settings.Value + pair.left + pair.right +
        values.Get(1) + lookup.Get(4);
}

int Arguments()
{
    var recorder = new Recorder();
    int first = recorder.Capture(3);
    int second = recorder.Capture(second: 2, first: 1, 5, 7);
    int ordered = recorder.Capture(
        second: recorder.Next(1), first: recorder.Next(2));
    var box = new OptionalBox();
    return first + second + ordered + recorder.stamp + box.Read();
}

int PatternIf(bool derived)
{
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    if (value is DerivedValue item) return item.Read();
    return value is null ? -2 : -1;
}

int PatternSwitch(bool derived)
{
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    switch (value)
    {
        case DerivedValue item when item.Read() == 41: return item.Read();
        case OtherValue other: return 2;
        default: return -1;
    }
}

int SwitchExpressions(int input, bool derived)
{
    int constant = input switch { 1 => 10, 2 when true => 20, _ => 30 };
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    int typed = value switch {
        DerivedValue item when item.Read() == 41 => item.Read(),
        OtherValue other => 2,
        _ => -1
    };
    return constant + typed;
}

void Fail(int code)
{
    throw new DerivedError { code = code };
}

int CatchAndFinally(bool fail)
{
    int value = 1;
    try
    {
        value = 2;
        if (fail) Fail(7);
        value = 3;
    }
    catch (OtherError other) { value = 900; }
    catch (ScriptError error) { value = error.code + 10; }
    finally { value = value + 100; }
    return value;
}

int RethrowAndFinally()
{
    int value = 0;
    try
    {
        try { Fail(5); }
        catch (ScriptError inner)
        {
            value = inner.code;
            throw;
        }
    }
    catch (ScriptError outer) { value = value + outer.code * 10; }
    finally { value = value + 100; }
    return value;
}

int ReturnRunsFinally()
{
    var probe = new CleanupProbe { value = 5 };
    int returned = probe.ReturnFromTry();
    return returned * 10 + probe.value;
}

int LoopCleanup()
{
    int cleaned = 0;
    int index = 0;
    while (index < 4)
    {
        index = index + 1;
        try
        {
            if (index == 1) continue;
            if (index == 3) break;
            cleaned = cleaned + 10;
        }
        finally { cleaned = cleaned + index; }
    }
    return cleaned;
}

void Uncaught() { Fail(1); }
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 24 var fixture compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    require(build.modules.size() == 1, "Phase 24 var module is missing");
    const auto mirText = realscript::mir::printModule(build.modules.front());
    require(mirText.find("throw %") != std::string::npos &&
            mirText.find("handler catch") != std::string::npos,
        "Phase 24 exception regions are missing from typed MIR output");
    bool handlerHasSourcePoint = false;
    for (const auto& function : build.modules.front().functions) {
        for (const auto& handler : function.exceptionHandlers) {
            handlerHasSourcePoint = handlerHasSourcePoint || std::any_of(
                function.debugInfo.sequencePoints.begin(),
                function.debugInfo.sequencePoints.end(),
                [&](const auto& point) {
                    return point.blockId == handler.handlerBlock;
                });
        }
    }
    require(handlerHasSourcePoint,
        "Phase 24 catch/finally handlers lost DAP source sequence points");

    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::diagnostics::DiagnosticBag verifierDiagnostics;
    const auto bytecodeVerified =
        realscript::bytecode::verifyModule(module, verifierDiagnostics);
    require(bytecodeVerified,
        "Phase 24 var bytecode verification failed:\n" +
            diagnosticsText(verifierDiagnostics) +
            "diagnostic count=" +
            std::to_string(verifierDiagnostics.items().size()) +
            ", version=" + std::to_string(module.version.major) + "." +
            std::to_string(module.version.minor));
    const auto encoded = realscript::bytecode::encodeModule(module);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag decodeDiagnostics;
    require(realscript::bytecode::decodeModule(
                encoded, decoded, decodeDiagnostics),
        "Phase 24 bytecode codec rejected the new expression opcodes:\n" +
            diagnosticsText(decodeDiagnostics));
    require(realscript::bytecode::encodeModule(decoded) == encoded,
        "Phase 24 bytecode did not round-trip canonically");
    const auto disassembly = realscript::bytecode::disassembleModule(decoded);
    require(disassembly.find("is.type") != std::string::npos &&
            disassembly.find("as.type") != std::string::npos &&
            disassembly.find("const.typeid") != std::string::npos &&
            disassembly.find("throw") != std::string::npos &&
            disassembly.find("handler catch") != std::string::npos,
        "Phase 24 type operations are missing from bytecode disassembly");
    realscript::runtime::Interpreter interpreter({std::move(decoded)});
    const auto result = interpreter.invoke(
        "Phase24.Core::InferLocals", {std::int64_t{5}});
    require(result.succeeded && std::get<std::int64_t>(result.value) == 10,
        "implicitly typed locals lost their inferred runtime types");
    const auto lazy = interpreter.invoke(
        "Phase24.Core::ConditionalIsLazy", {true, std::int64_t{0}});
    require(lazy.succeeded && std::get<std::int64_t>(lazy.value) == 7,
        "conditional expression eagerly evaluated its unselected arm");
    const auto selected = interpreter.invoke(
        "Phase24.Core::ConditionalReference", {true});
    const auto empty = interpreter.invoke(
        "Phase24.Core::ConditionalReference", {false});
    require(selected.succeeded && empty.succeeded &&
            std::holds_alternative<realscript::runtime::ObjectRef>(
                selected.value) &&
            std::holds_alternative<realscript::runtime::NullObject>(
                empty.value),
        "conditional expression lost its reference/null common type");
    const auto coalescedExisting = interpreter.invoke(
        "Phase24.Core::CoalesceHolder",
        {std::get<realscript::runtime::ObjectRef>(selected.value),
         std::int64_t{0}});
    const auto coalescedFallback = interpreter.invoke(
        "Phase24.Core::CoalesceHolder",
        {realscript::runtime::NullObject{}, std::int64_t{2}});
    require(coalescedExisting.succeeded && coalescedFallback.succeeded &&
            std::get<std::int64_t>(coalescedExisting.value) == 11 &&
            std::get<std::int64_t>(coalescedFallback.value) == 5,
        "null-coalescing operator lost lazy reference fallback semantics");
    const auto conditionalCallPresent = interpreter.invoke(
        "Phase24.Core::NullConditionalCall", {true});
    const auto conditionalCallMissing = interpreter.invoke(
        "Phase24.Core::NullConditionalCall", {false});
    const auto conditionalFieldPresent = interpreter.invoke(
        "Phase24.Core::NullConditionalField", {true});
    const auto conditionalFieldMissing = interpreter.invoke(
        "Phase24.Core::NullConditionalField", {false});
    const auto conditionalValuePresent = interpreter.invoke(
        "Phase24.Core::NullConditionalValue", {true});
    const auto conditionalValueMissing = interpreter.invoke(
        "Phase24.Core::NullConditionalValue", {false});
    const auto liftedPresent = interpreter.invoke(
        "Phase24.Core::LiftedNullableMembers", {true});
    const auto liftedMissing = interpreter.invoke(
        "Phase24.Core::LiftedNullableMembers", {false});
    require(conditionalCallPresent.succeeded &&
            conditionalCallMissing.succeeded &&
            conditionalFieldPresent.succeeded &&
            conditionalFieldMissing.succeeded &&
            conditionalValuePresent.succeeded &&
            conditionalValueMissing.succeeded &&
            liftedPresent.succeeded && liftedMissing.succeeded &&
            std::get<std::int64_t>(conditionalCallPresent.value) == 17 &&
            std::get<std::int64_t>(conditionalCallMissing.value) == 9 &&
            std::get<std::int64_t>(conditionalFieldPresent.value) == 23 &&
            std::get<std::int64_t>(conditionalFieldMissing.value) == 6 &&
            std::get<std::int64_t>(conditionalValuePresent.value) == 29 &&
            std::get<std::int64_t>(conditionalValueMissing.value) == 31,
        "null-conditional reference access lost call or field semantics");
    require(std::get<bool>(liftedPresent.value) &&
            std::get<bool>(liftedMissing.value),
        "lifted null-conditional value lost nullable member semantics");
    const auto isDerived = interpreter.invoke(
        "Phase24.Core::RuntimeIs", {true});
    const auto isOther = interpreter.invoke(
        "Phase24.Core::RuntimeIs", {false});
    const auto asDerived = interpreter.invoke(
        "Phase24.Core::RuntimeAs", {true});
    const auto asOther = interpreter.invoke(
        "Phase24.Core::RuntimeAs", {false});
    const auto typeTokens = interpreter.invoke(
        "Phase24.Core::TypeTokens");
    const auto initializers = interpreter.invoke(
        "Phase24.Core::Initializers");
    const auto argumentFeatures = interpreter.invoke(
        "Phase24.Core::Arguments");
    const auto patternIfTrue = interpreter.invoke(
        "Phase24.Core::PatternIf", {true});
    const auto patternIfFalse = interpreter.invoke(
        "Phase24.Core::PatternIf", {false});
    const auto patternSwitchTrue = interpreter.invoke(
        "Phase24.Core::PatternSwitch", {true});
    const auto patternSwitchFalse = interpreter.invoke(
        "Phase24.Core::PatternSwitch", {false});
    const auto switchExpressionTrue = interpreter.invoke(
        "Phase24.Core::SwitchExpressions", {std::int64_t{1}, true});
    const auto switchExpressionFalse = interpreter.invoke(
        "Phase24.Core::SwitchExpressions", {std::int64_t{2}, false});
    const auto caught = interpreter.invoke(
        "Phase24.Core::CatchAndFinally", {true});
    const auto noThrow = interpreter.invoke(
        "Phase24.Core::CatchAndFinally", {false});
    const auto rethrown = interpreter.invoke(
        "Phase24.Core::RethrowAndFinally");
    const auto returnCleanup = interpreter.invoke(
        "Phase24.Core::ReturnRunsFinally");
    const auto loopCleanup = interpreter.invoke(
        "Phase24.Core::LoopCleanup");
    require(isDerived.succeeded && isOther.succeeded &&
            asDerived.succeeded && asOther.succeeded && typeTokens.succeeded &&
            initializers.succeeded &&
            argumentFeatures.succeeded &&
            patternIfTrue.succeeded && patternIfFalse.succeeded &&
            patternSwitchTrue.succeeded && patternSwitchFalse.succeeded &&
            switchExpressionTrue.succeeded && switchExpressionFalse.succeeded &&
            caught.succeeded && noThrow.succeeded && rethrown.succeeded &&
            returnCleanup.succeeded && loopCleanup.succeeded &&
            std::get<bool>(isDerived.value) && !std::get<bool>(isOther.value) &&
            std::get<std::int64_t>(asDerived.value) == 41 &&
            std::get<std::int64_t>(asOther.value) == -1 &&
            std::get<bool>(typeTokens.value) &&
            std::get<std::int64_t>(initializers.value) == 71,
        "is/as/typeof lost runtime type identity semantics");
    require(std::get<std::int64_t>(argumentFeatures.value) == 43,
        "optional, named, or params argument semantics diverged");
    require(std::get<std::int64_t>(patternIfTrue.value) == 41 &&
            std::get<std::int64_t>(patternIfFalse.value) == -1 &&
            std::get<std::int64_t>(patternSwitchTrue.value) == 41 &&
            std::get<std::int64_t>(patternSwitchFalse.value) == 2 &&
            std::get<std::int64_t>(switchExpressionTrue.value) == 51 &&
            std::get<std::int64_t>(switchExpressionFalse.value) == 22,
        "pattern matching or switch expression semantics diverged");
    require(std::get<std::int64_t>(caught.value) == 117 &&
            std::get<std::int64_t>(noThrow.value) == 103 &&
            std::get<std::int64_t>(rethrown.value) == 155 &&
            std::get<std::int64_t>(returnCleanup.value) == 56 &&
            std::get<std::int64_t>(loopCleanup.value) == 16,
        "throw/catch/rethrow or finally control-flow semantics diverged");
    const auto uncaught = interpreter.invoke("Phase24.Core::Uncaught");
    require(!uncaught.succeeded &&
            uncaught.error.code ==
                realscript::runtime::ErrorCode::ScriptException,
        "uncaught script exception did not surface as ScriptException");
}

std::vector<realscript::bytecode::Module> compileModules(
    const std::string& source) {
    realscript::compiler::Compilation compilation({{"phase24_tools.rs", source}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 24 tooling fixture compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) {
        modules.push_back(lowerer.lower(module));
    }
    return modules;
}

void testToolingAndHotReload() {
    realscript::tooling::LanguageService service;
    service.open("phase24_completion.rs", R"(
module Phase24.Tooling;
int Run() { return 1; }
)");
    const auto completion = service.completion(
        "phase24_completion.rs", {2, 0});
    for (const auto* keyword : {
             "var", "params", "is", "as", "typeof", "when", "throw",
             "try", "catch", "finally"}) {
        require(std::any_of(
                completion.begin(), completion.end(),
                [&](const auto& item) { return item.label == keyword; }),
            std::string("LSP completion is missing Phase 24 keyword '") +
                keyword + "'");
    }

    const auto before = compileModules(R"(
module Phase24.Reload;
class FirstError { }
class SecondError { }
void Fail(FirstError value) { throw value; }
int Run(FirstError value)
{
    try { Fail(value); }
    catch (FirstError error) { return 1; }
    catch { return 2; }
    return 3;
}
)");
    const auto after = compileModules(R"(
module Phase24.Reload;
class FirstError { }
class SecondError { }
void Fail(FirstError value) { throw value; }
int Run(FirstError value)
{
    try { Fail(value); }
    catch (SecondError error) { return 1; }
    catch { return 2; }
    return 3;
}
)");
    realscript::runtime::RuntimeError error;
    auto current = realscript::runtime::ProgramImage::link(before, error);
    require(current.has_value(),
        "Phase 24 hot-reload baseline failed to link: " + error.message);
    const auto plan = realscript::hot_reload::prepare(*current, after);
    require(plan.compatible && !plan.changedFunctions.empty(),
        "hot reload did not fingerprint exception handler metadata");
}

void testImplicitLocalDiagnostics() {
    struct Case {
        const char* source;
        const char* expectedCode;
    };
    const std::vector<Case> cases = {
        {R"(module Phase24.VarMissing; void Bad() { var value; })", "RS8901"},
        {R"(module Phase24.VarNull; void Bad() { var value = null; })", "RS8902"},
        {R"(module Phase24.BadConditional; void Bad() { var value = true ? 1 : "x"; })", "RS8904"},
        {R"(module Phase24.BadCoalesce; void Bad() { var value = 1 ?? 2; })", "RS8905"},
        {R"(module Phase24.BadAs; void Bad() { var value = 1 as int; })", "RS8910"},
        {R"(module Phase24.BadInit; class A { public int x; } void Bad() { var value = new A { x = 1, x = 2 }; })", "RS8911"},
        {R"(module Phase24.BadParams; void Bad(params int value, int tail) { return; })", "RS8916"},
        {R"(module Phase24.BadOptional; void Bad(int first = 1, int second) { return; })", "RS8919"},
        {R"(module Phase24.BadSwitchExpr; int Bad(int value) { return value switch { 1 => 2 }; })", "RS8922"},
        {R"(module Phase24.BadRethrow; void Bad() { throw; })", "RS8927"},
        {R"(module Phase24.BadCatch; void Bad() { try { } catch (int value) { } })", "RS8929"},
        {R"(module Phase24.BadFinallyReturn; int Bad() { try { return 1; } finally { return 2; } })", "RS8930"},
        {R"(module Phase24.BadTry; void Bad() { try { } })", "RS1120"},
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        realscript::compiler::Compilation compilation({{
            "phase24_var_bad_" + std::to_string(index) + ".rs",
            cases[index].source,
        }});
        const auto build = compilation.build();
        require(hasDiagnostic(build.diagnostics, cases[index].expectedCode),
            "missing expected var diagnostic " +
                std::string(cases[index].expectedCode) + ":\n" +
                diagnosticsText(build.diagnostics));
    }
}

} // namespace

int main() {
    try {
        testImplicitLocalTypes();
        testImplicitLocalDiagnostics();
        testToolingAndHotReload();
        std::cout << "Phase 24 language completeness tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
