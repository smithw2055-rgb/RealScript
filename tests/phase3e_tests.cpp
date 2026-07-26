#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/semantic/Semantic.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

realscript::compiler::BuildResult build(
    const std::vector<realscript::compiler::SourceFile>& files) {
    realscript::compiler::Compilation compilation;
    for (const auto& file : files) compilation.addSource(file);
    return compilation.build();
}

std::vector<realscript::bytecode::Module> compile(
    const std::vector<realscript::compiler::SourceFile>& files) {
    auto result = build(files);
    if (result.diagnostics.hasErrors()) {
        std::string message = "source compilation failed";
        for (const auto& diagnostic : result.diagnostics.items()) {
            message += "\n" + diagnostic.code + ": " + diagnostic.message;
        }
        throw std::runtime_error(message);
    }
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& mir : result.modules) {
        auto module = lowerer.lower(mir);
        realscript::diagnostics::DiagnosticBag diagnostics;
        require(realscript::bytecode::verifyModule(module, diagnostics),
            "Phase 3E bytecode verification failed");
        modules.push_back(std::move(module));
    }
    return modules;
}

bool hasDiagnostic(
    const realscript::compiler::BuildResult& result,
    const std::string& code) {
    for (const auto& diagnostic : result.diagnostics.items()) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

const char* valueSource = R"(
module Phase3E.Values;

enum Team
{
    Neutral,
    Player = 5,
    Enemy
}

struct Vector2
{
    double x;
    double y;

    Vector2(double x, double y)
    {
        this.x = x;
        this.y = y;
    }

    double LengthSquared()
    {
        return x * x + y * y;
    }

    double X { get { return x; } }
}

long longMath()
{
    long value = 2147483648;
    return value + 2;
}

double numericPromotion()
{
    int a = 2;
    long b = 3;
    double c = 0.5;
    return a + b + c;
}

bool enumValues()
{
    Team team = Team.Enemy;
    return Team.Neutral != Team.Player && team == Team.Enemy;
}

double structCopy()
{
    Vector2 value = new Vector2(3.0, 4.0);
    Vector2 copy = value;
    copy.x = 6.0;
    return value.LengthSquared() + copy.X;
}

double structArray()
{
    Vector2[] values = new Vector2[1];
    values[0] = new Vector2(2.0, 5.0);
    return values[0].LengthSquared();
}

double defaultStruct()
{
    Vector2 value = new Vector2();
    return value.x + value.y;
}

double ieeeDivision()
{
    return 1.0 / 0.0;
}

long longOverflow()
{
    long value = 9223372036854775807;
    return value + 1;
}
)";

void testNumericEnumAndStructExecution() {
    auto modules = compile({{"values.rs", valueSource}});
    require(modules.size() == 1, "expected one Phase 3E module");
    const auto& module = modules.front();
    require(module.version.major == 0 && module.version.minor == 4,
        "Phase 3E must use .rsbc 0.4");

    bool foundStruct = false;
    bool foundEnum = false;
    for (const auto& type : module.types) {
        foundStruct = foundStruct ||
            type.kind == realscript::semantic::TypeKind::Struct;
        foundEnum = foundEnum ||
            type.kind == realscript::semantic::TypeKind::Enum;
    }
    require(foundStruct && foundEnum,
        "struct or enum descriptors were not emitted");

    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto longResult = interpreter.invoke("Phase3E.Values::longMath");
    const auto numeric = interpreter.invoke("Phase3E.Values::numericPromotion");
    const auto enumResult = interpreter.invoke("Phase3E.Values::enumValues");
    const auto copy = interpreter.invoke("Phase3E.Values::structCopy");
    const auto array = interpreter.invoke("Phase3E.Values::structArray");
    const auto defaults = interpreter.invoke("Phase3E.Values::defaultStruct");
    const auto ieee = interpreter.invoke("Phase3E.Values::ieeeDivision");
    const auto overflow = interpreter.invoke("Phase3E.Values::longOverflow");
    require(longResult.succeeded &&
            std::get<realscript::runtime::LongValue>(longResult.value).value ==
                2147483650ll,
        "long arithmetic returned the wrong result or lost its runtime type");
    require(numeric.succeeded &&
            std::abs(std::get<double>(numeric.value) - 5.5) < 1e-12,
        "numeric promotion returned the wrong result");
    require(enumResult.succeeded && std::get<bool>(enumResult.value),
        "enum identity or explicit values are incorrect");
    require(copy.succeeded &&
            std::abs(std::get<double>(copy.value) - 31.0) < 1e-12,
        "struct copy-on-write value semantics are incorrect");
    require(array.succeeded &&
            std::abs(std::get<double>(array.value) - 29.0) < 1e-12,
        "struct arrays or instance methods returned the wrong result");
    require(defaults.succeeded && std::get<double>(defaults.value) == 0.0,
        "implicit zero-initialized struct construction failed");
    require(ieee.succeeded && std::isinf(std::get<double>(ieee.value)),
        "double division did not preserve IEEE 754 infinity semantics");
    require(!overflow.succeeded &&
            overflow.error.code == realscript::runtime::ErrorCode::IntegerOverflow,
        "long overflow did not produce a checked runtime error");
}

void testNestedStructReferencesArePreciseRoots() {
    auto modules = compile({{"graph.rs", R"(
module Phase3E.Graph;
class Node { int value; }
struct Holder
{
    Node node;
    Holder(Node node) { this.node = node; }
    Node NodeValue { get { return node; } }
}
Holder create()
{
    Node node = new Node();
    node.value = 9;
    return new Holder(node);
}
)"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Phase3E.Graph::create");
    require(result.succeeded &&
            std::holds_alternative<realscript::runtime::StructValue>(result.value),
        "nested reference fixture did not return a struct");
    const auto structure = std::get<realscript::runtime::StructValue>(result.value);
    require(structure.storage && structure.storage->fields.size() == 1 &&
            std::holds_alternative<realscript::runtime::ObjectRef>(
                structure.storage->fields.front()),
        "struct did not retain its object field");
    const auto child = std::get<realscript::runtime::ObjectRef>(
        structure.storage->fields.front());

    realscript::runtime::ManagedHeap foreignHeap;
    auto invalidRoot = foreignHeap.retain(result.value);
    require(!invalidRoot.valid(),
        "cross-heap nested struct root was accepted");

    realscript::runtime::ShadowStack roots;
    auto persistent = interpreter.heap()->retain(result.value);
    require(persistent.valid() && !persistent.reference().valid() &&
            std::holds_alternative<realscript::runtime::StructValue>(
                persistent.value()),
        "host could not retain a struct value containing managed references");
    interpreter.heap()->collectFull(roots);
    require(interpreter.heap()->isAlive(child),
        "persistent struct root did not retain its nested object");
    const auto persistentSnapshot = interpreter.heap()->snapshot();
    require(persistentSnapshot.roots.size() == 1 &&
            persistentSnapshot.roots.front().kind == "persistent",
        "heap snapshot did not expose the persistent struct root");
    persistent.reset();

    std::vector<realscript::runtime::Value> locals{result.value};
    roots.pushFrame(nullptr, &locals, nullptr);
    interpreter.heap()->collectFull(roots);
    require(interpreter.heap()->isAlive(child),
        "GC did not scan references nested inside a shadow-stack struct root");
    const auto snapshot = interpreter.heap()->snapshot(&roots);
    require(snapshot.roots.size() == 1 &&
            snapshot.roots.front().kind == "shadow",
        "heap snapshot did not expose the nested shadow-stack struct root");
    roots.popFrame();
    locals.clear();
    interpreter.heap()->collectFull(roots);
    require(!interpreter.heap()->isAlive(child),
        "object nested in an unrooted struct was not reclaimed");
}

void testValueTypeDiagnostics() {
    const auto recursive = build({{"recursive.rs", R"(
module Phase3E.Invalid;
struct A { B value; }
struct B { A value; }
)"}});
    require(recursive.diagnostics.hasErrors() &&
            hasDiagnostic(recursive, "RS2487"),
        "recursive struct layout was not rejected");

    const auto mutation = build({{"mutation.rs", R"(
module Phase3E.InvalidMutation;
struct Counter
{
    int value;
    void Increment() { this.value = this.value + 1; }
}
)"}});
    require(mutation.diagnostics.hasErrors() &&
            hasDiagnostic(mutation, "RS2485"),
        "mutating struct instance method was silently accepted");

    const auto setter = build({{"setter.rs", R"(
module Phase3E.InvalidProperty;
struct Item { int Value { get; set; } }
)"}});
    require(setter.diagnostics.hasErrors() &&
            hasDiagnostic(setter, "RS2486"),
        "struct property setter was silently accepted");


    const auto enumOverflow = build({{"enum-overflow.rs", R"(
module Phase3E.InvalidEnum;
enum Limits
{
    Maximum = 9223372036854775807,
    Overflow
}
)"}});
    require(enumOverflow.diagnostics.hasErrors() &&
            hasDiagnostic(enumOverflow, "RS2451"),
        "implicit enum value overflow was not rejected");
}

void testValueCodecAndVerifier() {
    auto module = compile({{"values.rs", valueSource}}).front();
    const auto encoded = realscript::bytecode::encodeModule(module);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(realscript::bytecode::decodeModule(encoded, decoded, diagnostics),
        "Phase 3E bytecode failed to decode");
    require(encoded == realscript::bytecode::encodeModule(decoded),
        "Phase 3E bytecode round trip is not canonical");

    auto descriptorCorruption = decoded;
    require(!descriptorCorruption.types.empty(),
        "Phase 3E bytecode fixture had no type descriptor");
    ++descriptorCorruption.types.front().id;
    realscript::diagnostics::DiagnosticBag descriptorDiagnostics;
    require(!realscript::bytecode::verifyModule(
                descriptorCorruption,
                descriptorDiagnostics),
        "verifier accepted a descriptor whose TypeId did not match its canonical name");

    bool corrupted = false;
    for (auto& function : decoded.functions) {
        for (std::size_t index = 0; index < function.registerTypes.size(); ++index) {
            if (function.registerTypes[index] ==
                    realscript::semantic::PrimitiveType::Struct) {
                function.registerTypeIds[index] = 0;
                corrupted = true;
                break;
            }
        }
        if (corrupted) break;
    }
    require(corrupted, "struct bytecode fixture had no struct register");
    realscript::diagnostics::DiagnosticBag verifyDiagnostics;
    require(!realscript::bytecode::verifyModule(decoded, verifyDiagnostics),
        "verifier accepted a struct register without an exact TypeId");
}

} // namespace

int main() {
    try {
        testNumericEnumAndStructExecution();
        testNestedStructReferencesArePreciseRoots();
        testValueTypeDiagnostics();
        testValueCodecAndVerifier();
        std::cout << "Phase 3E tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
