#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/GameProductization.h"
#include "realscript/game/GameScripting.h"
#include "realscript/runtime/Runtime.h"

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

void testExactNumericIdentityAndWidening() {
    realscript::compiler::Compilation compilation({{"phase23.rs", R"(
module Phase23.Values;

byte EchoByte(byte value) { return value; }
sbyte EchoSByte(sbyte value) { return value; }
short EchoShort(short value) { return value; }
ushort EchoUShort(ushort value) { return value; }
uint EchoUInt(uint value) { return value; }
ulong EchoULong(ulong value) { return value; }
float EchoFloat(float value) { return value; }
char EchoChar(char value) { return value; }

long WidenByte(byte value) { return value; }
ulong WidenUInt(uint value) { return value; }
double WidenFloat(float value) { return value; }
uint AddUInt(uint left, uint right) { return left + right; }
uint AddUIntUnchecked(uint left, uint right) { return unchecked(left + right); }
ulong SubtractULong(ulong left, ulong right) { return left - right; }
float MultiplyFloat(float left, float right) { return left * right; }
int PromoteBytes(byte left, byte right) { return left + right; }
byte CastByteChecked(int value) { return checked((byte)value); }
byte CastByteUnchecked(int value) { return unchecked((byte)value); }

struct MutableCounter
{
    int value;
    MutableCounter(int initial) { this.value = initial; }
    void Add(int amount) { this.value = this.value + amount; }
    void AddViaThis(int amount) { this.Add(amount); }
    int Read() { return this.value; }
}

int MutateStruct(int initial)
{
    MutableCounter counter = new MutableCounter(initial);
    counter.Add(3);
    counter.Add(4);
    return counter.Read();
}

int MutateStructViaExplicitThis(int initial)
{
    MutableCounter counter = new MutableCounter(initial);
    counter.AddViaThis(9);
    return counter.Read();
}

class StructHolder
{
    public MutableCounter counter;
    public StructHolder(int initial)
    {
        counter = new MutableCounter(initial);
    }
}

int MutateStructField(int initial)
{
    StructHolder holder = new StructHolder(initial);
    holder.counter.Add(7);
    return holder.counter.Read();
}

int MutateStructIndexer(int initial)
{
    MutableCounter[] values = new MutableCounter[1];
    values[0] = new MutableCounter(initial);
    values[0].Add(8);
    return values[0].Read();
}

void Increment(ref int value) { value = value + 1; }

class IntHolder
{
    public int value;
    public IntHolder(int initial) { value = initial; }
}

int RefFieldWriteback(int initial)
{
    IntHolder holder = new IntHolder(initial);
    Increment(ref holder.value);
    return holder.value;
}

int RefIndexerWriteback(int initial)
{
    int[] values = new int[1];
    values[0] = initial;
    Increment(ref values[0]);
    return values[0];
}

int RefLocalWriteback(int initial)
{
    int value = initial;
    ref int alias = ref value;
    alias = alias + 2;
    return value;
}

int NullableRoundTrip(bool hasValue)
{
    int? value = null;
    if (hasValue)
    {
        value = 7;
    }
    if (value.HasValue())
    {
        return value.Value();
    }
    return -1;
}

Box<int> BoxInt(int value) { return new Box<int>(value); }
int UnboxInt(Box<int> value) { return value.Value(); }
int BoxRoundTrip(int value) { return UnboxInt(BoxInt(value)); }

int ReadAfterMutatingIn(in MutableCounter value)
{
    value.Add(5);
    return value.Read();
}

int InDefensiveCopy()
{
    MutableCounter value = new MutableCounter(2);
    int observed = ReadAfterMutatingIn(in value);
    return observed * 10 + value.Read();
}

ref int ForwardReference(ref int value) { return ref value; }

void MutateThroughRefReturn(ref int value)
{
    ref int alias = ref ForwardReference(ref value);
    alias = alias + 3;
}

int RefReturnWriteback(int initial)
{
    int value = initial;
    MutateThroughRefReturn(ref value);
    return value;
}

class ExactFields
{
    byte byteValue;
    ulong ulongValue;
    float floatValue;
    char charValue;

    public ExactFields(byte b, ulong u, float f, char c)
    {
        byteValue = b;
        ulongValue = u;
        floatValue = f;
        charValue = c;
    }

    public void Set(byte b, ulong u, float f, char c)
    {
        byteValue = b;
        ulongValue = u;
        floatValue = f;
        charValue = c;
    }

    public byte ByteValue() { return byteValue; }
    public ulong ULongValue() { return ulongValue; }
    public float FloatValue() { return floatValue; }
    public char CharValue() { return charValue; }
}

ExactFields CreateExactFields(byte b, ulong u, float f, char c)
{
    return new ExactFields(b, u, f, c);
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 23 exact numeric compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    require(build.modules.size() == 1, "Phase 23 module is missing");

    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::diagnostics::DiagnosticBag verifierDiagnostics;
    require(realscript::bytecode::verifyModule(module, verifierDiagnostics),
        "Phase 23 bytecode verification failed:\n" +
            diagnosticsText(verifierDiagnostics));
    const auto encoded = realscript::bytecode::encodeModule(module);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag decodeDiagnostics;
    require(realscript::bytecode::decodeModule(
            encoded, decoded, decodeDiagnostics),
        "Phase 23 bytecode decode failed:\n" +
            diagnosticsText(decodeDiagnostics));
    require(realscript::bytecode::encodeModule(decoded) == encoded,
        "Phase 23 exact numeric bytecode is not canonical");

    realscript::runtime::Interpreter interpreter({decoded});
    const auto echoByte = interpreter.invoke(
        "Phase23.Values::EchoByte",
        {realscript::runtime::ByteValue{42}});
    require(echoByte.succeeded &&
            std::get<realscript::runtime::ByteValue>(echoByte.value).value == 42,
        "byte lost its exact runtime identity");
    const auto echoULong = interpreter.invoke(
        "Phase23.Values::EchoULong",
        {realscript::runtime::ULongValue{0xf000000000000001ULL}});
    require(echoULong.succeeded &&
            std::get<realscript::runtime::ULongValue>(echoULong.value).value ==
                0xf000000000000001ULL,
        "ulong lost its exact unsigned runtime value");
    const auto echoFloat = interpreter.invoke(
        "Phase23.Values::EchoFloat",
        {realscript::runtime::FloatValue{1.25f}});
    require(echoFloat.succeeded &&
            std::get<realscript::runtime::FloatValue>(echoFloat.value).value == 1.25f,
        "float lost its exact runtime identity");
    const auto echoChar = interpreter.invoke(
        "Phase23.Values::EchoChar",
        {realscript::runtime::CharValue{u'Z'}});
    require(echoChar.succeeded &&
            std::get<realscript::runtime::CharValue>(echoChar.value).value == u'Z',
        "char lost its exact runtime identity");

    const auto widenedByte = interpreter.invoke(
        "Phase23.Values::WidenByte",
        {realscript::runtime::ByteValue{250}});
    require(widenedByte.succeeded &&
            std::get<realscript::runtime::LongValue>(widenedByte.value).value == 250,
        "byte-to-long widening conversion failed");
    const auto widenedUInt = interpreter.invoke(
        "Phase23.Values::WidenUInt",
        {realscript::runtime::UIntValue{4000000000U}});
    require(widenedUInt.succeeded &&
            std::get<realscript::runtime::ULongValue>(widenedUInt.value).value ==
                4000000000ULL,
        "uint-to-ulong widening conversion failed");
    const auto widenedFloat = interpreter.invoke(
        "Phase23.Values::WidenFloat",
        {realscript::runtime::FloatValue{3.5f}});
    require(widenedFloat.succeeded &&
            std::get<double>(widenedFloat.value) == 3.5,
        "float-to-double widening conversion failed");

    const auto addUInt = interpreter.invoke(
        "Phase23.Values::AddUInt",
        {realscript::runtime::UIntValue{3000000000U},
         realscript::runtime::UIntValue{1000000000U}});
    require(addUInt.succeeded &&
            std::get<realscript::runtime::UIntValue>(addUInt.value).value ==
                4000000000U,
        "uint arithmetic did not preserve unsigned identity");
    const auto overflowUInt = interpreter.invoke(
        "Phase23.Values::AddUInt",
        {realscript::runtime::UIntValue{4000000000U},
         realscript::runtime::UIntValue{4000000000U}});
    require(!overflowUInt.succeeded &&
            overflowUInt.error.code ==
                realscript::runtime::ErrorCode::IntegerOverflow,
        "checked uint overflow did not fail deterministically");
    const auto wrappedUInt = interpreter.invoke(
        "Phase23.Values::AddUIntUnchecked",
        {realscript::runtime::UIntValue{4000000000U},
         realscript::runtime::UIntValue{4000000000U}});
    require(wrappedUInt.succeeded &&
            std::get<realscript::runtime::UIntValue>(wrappedUInt.value).value ==
                3705032704U,
        "unchecked uint arithmetic did not wrap deterministically");
    const auto subtractULong = interpreter.invoke(
        "Phase23.Values::SubtractULong",
        {realscript::runtime::ULongValue{0xf000000000000010ULL},
         realscript::runtime::ULongValue{0xf000000000000000ULL}});
    require(subtractULong.succeeded &&
            std::get<realscript::runtime::ULongValue>(subtractULong.value).value == 16,
        "ulong arithmetic used signed semantics");
    const auto multiplyFloat = interpreter.invoke(
        "Phase23.Values::MultiplyFloat",
        {realscript::runtime::FloatValue{1.5f},
         realscript::runtime::FloatValue{2.0f}});
    require(multiplyFloat.succeeded &&
            std::get<realscript::runtime::FloatValue>(multiplyFloat.value).value == 3.0f,
        "float arithmetic widened its runtime identity");
    const auto promotedBytes = interpreter.invoke(
        "Phase23.Values::PromoteBytes",
        {realscript::runtime::ByteValue{200},
         realscript::runtime::ByteValue{55}});
    require(promotedBytes.succeeded &&
            std::get<std::int64_t>(promotedBytes.value) == 255,
        "small integral arithmetic did not promote to int");
    const auto checkedCast = interpreter.invoke(
        "Phase23.Values::CastByteChecked", {std::int64_t{300}});
    require(!checkedCast.succeeded &&
            checkedCast.error.code ==
                realscript::runtime::ErrorCode::IntegerOverflow,
        "checked narrowing conversion did not report overflow");
    const auto uncheckedCast = interpreter.invoke(
        "Phase23.Values::CastByteUnchecked", {std::int64_t{300}});
    require(uncheckedCast.succeeded &&
            std::get<realscript::runtime::ByteValue>(uncheckedCast.value).value == 44,
        "unchecked narrowing conversion did not wrap deterministically");
    const auto mutableStruct = interpreter.invoke(
        "Phase23.Values::MutateStruct", {std::int64_t{5}});
    require(mutableStruct.succeeded &&
            std::get<std::int64_t>(mutableStruct.value) == 12,
        "mutable struct receiver did not write its value back to the caller");
    const auto mutableStructViaExplicitThis = interpreter.invoke(
        "Phase23.Values::MutateStructViaExplicitThis", {std::int64_t{5}});
    require(mutableStructViaExplicitThis.succeeded &&
            std::get<std::int64_t>(mutableStructViaExplicitThis.value) == 14,
        "explicit this mutable struct receiver lost its writeback target");
    const auto mutableStructField = interpreter.invoke(
        "Phase23.Values::MutateStructField", {std::int64_t{5}});
    require(mutableStructField.succeeded &&
            std::get<std::int64_t>(mutableStructField.value) == 12,
        "mutable struct field receiver did not write back through its owner");
    const auto mutableStructIndexer = interpreter.invoke(
        "Phase23.Values::MutateStructIndexer", {std::int64_t{5}});
    require(mutableStructIndexer.succeeded &&
            std::get<std::int64_t>(mutableStructIndexer.value) == 13,
        "mutable struct indexer receiver did not write back to its array");
    const auto refField = interpreter.invoke(
        "Phase23.Values::RefFieldWriteback", {std::int64_t{5}});
    require(refField.succeeded &&
            std::get<std::int64_t>(refField.value) == 6,
        "ref field target did not write back through its owner");
    const auto refIndexer = interpreter.invoke(
        "Phase23.Values::RefIndexerWriteback", {std::int64_t{5}});
    require(refIndexer.succeeded &&
            std::get<std::int64_t>(refIndexer.value) == 6,
        "ref indexer target did not write back to its array");
    const auto refLocal = interpreter.invoke(
        "Phase23.Values::RefLocalWriteback", {std::int64_t{5}});
    require(refLocal.succeeded &&
            std::get<std::int64_t>(refLocal.value) == 7,
        "ref local did not preserve variable alias identity");
    const auto nullableValue = interpreter.invoke(
        "Phase23.Values::NullableRoundTrip", {true});
    const auto nullableEmpty = interpreter.invoke(
        "Phase23.Values::NullableRoundTrip", {false});
    require(nullableValue.succeeded && nullableEmpty.succeeded &&
            std::get<std::int64_t>(nullableValue.value) == 7 &&
            std::get<std::int64_t>(nullableEmpty.value) == -1,
        "nullable value lost HasValue/Value/default semantics");
    const auto boxed = interpreter.invoke(
        "Phase23.Values::BoxRoundTrip", {std::int64_t{23}});
    require(boxed.succeeded &&
            std::get<std::int64_t>(boxed.value) == 23,
        "boxed value did not preserve its exact payload during unboxing");
    const auto defensiveCopy = interpreter.invoke(
        "Phase23.Values::InDefensiveCopy");
    require(defensiveCopy.succeeded &&
            std::get<std::int64_t>(defensiveCopy.value) == 22,
        "mutating an in struct receiver escaped its defensive copy");
    const auto refReturn = interpreter.invoke(
        "Phase23.Values::RefReturnWriteback", {std::int64_t{5}});
    require(refReturn.succeeded &&
            std::get<std::int64_t>(refReturn.value) == 8,
        "ref return did not preserve alias identity through a call chain: " +
            (refReturn.succeeded
                ? std::to_string(std::get<std::int64_t>(refReturn.value))
                : refReturn.error.message));

    const auto created = interpreter.invoke(
        "Phase23.Values::CreateExactFields",
        {realscript::runtime::ByteValue{231},
         realscript::runtime::ULongValue{0xf123456789abcdefULL},
         realscript::runtime::FloatValue{6.25f},
         realscript::runtime::CharValue{u'\u754c'}});
    require(created.succeeded &&
            std::holds_alternative<realscript::runtime::ObjectRef>(created.value),
        "exact numeric field object allocation failed");
    const auto fields = std::get<realscript::runtime::ObjectRef>(created.value);
    auto fieldsRoot = interpreter.heap()->retain(fields);
    const auto heapSnapshot = interpreter.heap()->snapshot();

    const auto mutated = interpreter.invoke(
        "Phase23.Values::ExactFields.Set",
        {fields,
         realscript::runtime::ByteValue{1},
         realscript::runtime::ULongValue{2},
         realscript::runtime::FloatValue{3.0f},
         realscript::runtime::CharValue{u'X'}});
    require(mutated.succeeded, "exact numeric field mutation failed");
    realscript::runtime::ShadowStack noFrames;
    interpreter.heap()->collectFull(noFrames);
    require(interpreter.heap()->isAlive(fields),
        "persistent exact numeric field object was collected");

    realscript::runtime::RuntimeError restoreError;
    require(interpreter.heap()->restore(heapSnapshot, &restoreError),
        "exact numeric heap snapshot restore failed: " + restoreError.message);
    const auto get = [&](const std::string& name) {
        return interpreter.invoke(
            "Phase23.Values::ExactFields." + name, {fields});
    };
    const auto restoredByte = get("ByteValue");
    const auto restoredULong = get("ULongValue");
    const auto restoredFloat = get("FloatValue");
    const auto restoredChar = get("CharValue");
    require(restoredByte.succeeded &&
            std::get<realscript::runtime::ByteValue>(restoredByte.value).value == 231,
        "byte field lost identity across GC/snapshot restore");
    require(restoredULong.succeeded &&
            std::get<realscript::runtime::ULongValue>(restoredULong.value).value ==
                0xf123456789abcdefULL,
        "ulong field lost identity across GC/snapshot restore");
    require(restoredFloat.succeeded &&
            std::get<realscript::runtime::FloatValue>(restoredFloat.value).value == 6.25f,
        "float field lost identity across GC/snapshot restore");
    require(restoredChar.succeeded &&
            std::get<realscript::runtime::CharValue>(restoredChar.value).value == u'\u754c',
        "char field lost identity across GC/snapshot restore");
}

void testReferenceDiagnostics() {
    struct Case {
        const char* source;
        const char* expectedCode;
    };
    const std::vector<Case> cases = {
        {R"(
module Phase23.BadRefReturn;
ref int Bad(int value) { return ref value; }
)", "RS8828"},
        {R"(
module Phase23.BadReturnModifier;
int Bad(ref int value) { return ref value; }
)", "RS8829"},
        {R"(
module Phase23.MissingRefReturn;
ref int Bad(ref int value) { return value; }
)", "RS8827"},
        {R"(
module Phase23.InAlias;
void Bad(in int value) { ref int alias = ref value; }
)", "RS8824"},
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        realscript::compiler::Compilation compilation({{
            "phase23_bad_" + std::to_string(index) + ".rs",
            cases[index].source,
        }});
        const auto build = compilation.build();
        require(hasDiagnostic(build.diagnostics, cases[index].expectedCode),
            "missing expected reference diagnostic " +
                std::string(cases[index].expectedCode) + ":\n" +
                diagnosticsText(build.diagnostics));
    }
}

void testExactNativeBindingAbi() {
    realscript::game::GameApi api;
    require(api.function(
        "Phase23.Native", "EchoByte",
        [](std::uint8_t value) -> std::uint8_t { return value; }),
        "byte native binding registration failed");
    require(api.function(
        "Phase23.Native", "EchoULong",
        [](std::uint64_t value) -> std::uint64_t { return value; }),
        "ulong native binding registration failed");
    require(api.function(
        "Phase23.Native", "EchoFloat",
        [](float value) -> float { return value; }),
        "float native binding registration failed");
    require(api.function(
        "Phase23.Native", "EchoChar",
        [](char16_t value) -> char16_t { return value; }),
        "char native binding registration failed");

    realscript::game::GameScriptCompiler compiler(api);
    const auto compiled = compiler.compile({{"phase23_native.rs", R"(
module Phase23.NativeProbe;
import Phase23.Native;

class Probe
{
    byte ByteValue(byte value) { return EchoByte(value); }
    ulong ULongValue(ulong value) { return EchoULong(value); }
    float FloatValue(float value) { return EchoFloat(value); }
    char CharValue(char value) { return EchoChar(value); }
}
)"}});
    require(compiled.succeeded(),
        "Phase 23 native ABI fixture compilation failed:\n" +
            diagnosticsText(compiled.diagnostics));
    realscript::game::ScriptRuntime runtime(compiled.program);
    realscript::runtime::RuntimeError error;
    const auto probe = runtime.createObject(
        "Phase23.NativeProbe::Probe", error);
    require(probe.has_value(), "Phase 23 native ABI probe allocation failed");
    const auto invoke = [&](const std::string& name,
                            realscript::runtime::Value argument) {
        const auto method = runtime.findMethod(probe->type(), name, 1);
        require(method.has_value(), "missing Phase 23 native ABI method " + name);
        return runtime.invoke(*probe, *method, {std::move(argument)});
    };
    const auto byteResult = invoke(
        "ByteValue", realscript::runtime::ByteValue{211});
    require(byteResult.succeeded &&
            std::get<realscript::runtime::ByteValue>(byteResult.value).value == 211,
        "native byte ABI round trip failed");
    const auto ulongResult = invoke(
        "ULongValue", realscript::runtime::ULongValue{0xf000000000000001ULL});
    require(ulongResult.succeeded &&
            std::get<realscript::runtime::ULongValue>(ulongResult.value).value ==
                0xf000000000000001ULL,
        "native ulong ABI round trip failed");
    const auto floatResult = invoke(
        "FloatValue", realscript::runtime::FloatValue{4.75f});
    require(floatResult.succeeded &&
            std::get<realscript::runtime::FloatValue>(floatResult.value).value == 4.75f,
        "native float ABI round trip failed");
    const auto charResult = invoke(
        "CharValue", realscript::runtime::CharValue{u'\u03a9'});
    require(charResult.succeeded &&
            std::get<realscript::runtime::CharValue>(charResult.value).value == u'\u03a9',
        "native char ABI round trip failed");
}

void testExactScriptStateCodec() {
    realscript::game::ScriptObjectState state;
    state.canonicalTypeName = "Phase23.State::ExactNumbers";
    state.fields = {
        {"byteValue", realscript::runtime::ByteValue{255}},
        {"sbyteValue", realscript::runtime::SByteValue{-128}},
        {"shortValue", realscript::runtime::ShortValue{-32768}},
        {"ushortValue", realscript::runtime::UShortValue{65535}},
        {"uintValue", realscript::runtime::UIntValue{4000000000U}},
        {"ulongValue", realscript::runtime::ULongValue{0xfedcba9876543210ULL}},
        {"floatValue", realscript::runtime::FloatValue{7.75f}},
        {"charValue", realscript::runtime::CharValue{u'\u754c'}},
    };
    realscript::game::ScriptStatePolicy policy;
    policy.allowDoubles = true;
    realscript::runtime::RuntimeError error;
    const auto expectedHash = state.canonicalHash();
    const auto bytes = realscript::game::encodeScriptObjectState(
        state, error, policy);
    require(!bytes.empty(),
        "exact numeric script-state encoding failed: " + error.message);
    const auto decoded = realscript::game::decodeScriptObjectState(
        bytes, error, policy);
    require(decoded.has_value(),
        "exact numeric script-state decoding failed: " + error.message);
    require(decoded->canonicalHash() == expectedHash &&
            decoded->fields.size() == state.fields.size(),
        "exact numeric script-state hash changed after round trip");
    require(std::get<realscript::runtime::ByteValue>(
                decoded->fields[0].value).value == 255 &&
            std::get<realscript::runtime::SByteValue>(
                decoded->fields[1].value).value == -128 &&
            std::get<realscript::runtime::ShortValue>(
                decoded->fields[2].value).value == -32768 &&
            std::get<realscript::runtime::UShortValue>(
                decoded->fields[3].value).value == 65535 &&
            std::get<realscript::runtime::UIntValue>(
                decoded->fields[4].value).value == 4000000000U &&
            std::get<realscript::runtime::ULongValue>(
                decoded->fields[5].value).value == 0xfedcba9876543210ULL &&
            std::get<realscript::runtime::FloatValue>(
                decoded->fields[6].value).value == 7.75f &&
            std::get<realscript::runtime::CharValue>(
                decoded->fields[7].value).value == u'\u754c',
        "exact numeric script-state values lost their runtime identities");

    realscript::game::ScriptStatePolicy strictPolicy;
    error = {};
    require(realscript::game::encodeScriptObjectState(
                state, error, strictPolicy).empty() &&
            error.code ==
                realscript::runtime::ErrorCode::DeterminismViolation,
        "float script state bypassed the deterministic state policy");
}

} // namespace

int main() {
    try {
        testExactNumericIdentityAndWidening();
        testReferenceDiagnostics();
        testExactNativeBindingAbi();
        testExactScriptStateCodec();
        std::cout << "Phase 23 value semantics tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
