#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/runtime/Runtime.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<realscript::bytecode::Module> compile(
    const std::vector<realscript::compiler::SourceFile>& files) {
    realscript::compiler::Compilation compilation;
    for (const auto& file : files) compilation.addSource(file);
    auto result = compilation.build();
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
            "bytecode verification failed");
        modules.push_back(std::move(module));
    }
    return modules;
}

const char* pointSource = R"(
module Game.Objects;

class Point
{
    int x;
    int y;
}

int main()
{
    Point point = new Point();
    point.x = 40;
    point.y = 2;
    return point.x + point.y;
}
)";

void testTypeDescriptorAndObjectBytecode() {
    auto modules = compile({{"point.rs", pointSource}});
    require(modules.size() == 1, "expected one object module");
    const auto& module = modules.front();
    require(module.version.major == 0 && module.version.minor == 6,
        "object bytecode must use format 0.6");
    require(module.types.size() == 1, "class descriptor was not emitted");
    const auto& type = module.types.front();
    require(type.name == "Point" && type.fields.size() == 2,
        "Point descriptor is incorrect");
    require(type.fields[0].name == "x" &&
            type.fields[0].type == realscript::semantic::PrimitiveType::Int,
        "Point.x layout is incorrect");

    const auto disassembly = realscript::bytecode::disassembleModule(module);
    require(disassembly.find("new.object type0") != std::string::npos,
        "object allocation bytecode is missing");
    require(disassembly.find("check.notnull type0") != std::string::npos,
        "object null check bytecode is missing");
    require(disassembly.find("store.field type0.0") != std::string::npos,
        "field store bytecode is missing");
    require(disassembly.find("load.field type0.1") != std::string::npos,
        "field load bytecode is missing");
}

void testObjectExecutionAndFieldDefaults() {
    auto modules = compile({
        {"point.rs", pointSource},
        {"defaults.rs", R"(
module Game.Defaults;
class State { int count; bool enabled; string name; }
int count() { State state = new State(); return state.count; }
bool enabled() { State state = new State(); return state.enabled; }
bool nameIsNull() { State state = new State(); return state.name == null; }
)"},
    });
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto mainResult = interpreter.invoke("Game.Objects::main");
    require(mainResult.succeeded &&
            std::get<std::int64_t>(mainResult.value) == 42,
        "object field execution returned the wrong value");
    const auto count = interpreter.invoke("Game.Defaults::count");
    const auto enabled = interpreter.invoke("Game.Defaults::enabled");
    const auto nameIsNull = interpreter.invoke("Game.Defaults::nameIsNull");
    require(count.succeeded && std::get<std::int64_t>(count.value) == 0,
        "integer field default is not zero");
    require(enabled.succeeded && !std::get<bool>(enabled.value),
        "Boolean field default is not false");
    require(nameIsNull.succeeded && std::get<bool>(nameIsNull.value),
        "string field default is not null");
}

void testNullReferenceTrap() {
    auto modules = compile({{"null.rs", R"(
module Game.Nulls;
class Point { int x; }
int main() { Point point = null; return point.x; }
)"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Game.Nulls::main");
    require(!result.succeeded, "null field access must fail");
    require(result.error.code == realscript::runtime::ErrorCode::NullReference,
        "null field access produced the wrong runtime error");
}

void testObjectGraphUsesDescriptorReferenceMap() {
    auto modules = compile({{"node.rs", R"(
module Game.Nodes;
class Node { Node next; int value; }
Node make() {
    Node parent = new Node();
    Node child = new Node();
    parent.next = child;
    parent.value = 7;
    parent.next.value = 9;
    return parent;
}
)"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Game.Nodes::make");
    require(result.succeeded &&
            std::holds_alternative<realscript::runtime::ObjectRef>(result.value),
        "object graph function did not return a managed object");
    const auto parent = std::get<realscript::runtime::ObjectRef>(result.value);
    const auto childValue = interpreter.heap()->fieldGet(parent, 0);
    require(childValue &&
            std::holds_alternative<realscript::runtime::ObjectRef>(*childValue),
        "object reference field was not stored");
    const auto child = std::get<realscript::runtime::ObjectRef>(*childValue);

    const auto token = interpreter.heap()->addPersistentRoot(parent);
    require(token != 0, "object graph root registration failed");
    realscript::runtime::ShadowStack roots;
    interpreter.heap()->collectFull(roots);
    require(interpreter.heap()->isAlive(parent), "root object was reclaimed");
    require(interpreter.heap()->isAlive(child),
        "descriptor reference map did not retain the child object");
    require(interpreter.heap()->removePersistentRoot(token),
        "object graph root removal failed");
    interpreter.heap()->collectFull(roots);
    require(!interpreter.heap()->isAlive(parent) &&
            !interpreter.heap()->isAlive(child),
        "unrooted object graph was not reclaimed");
}

void testObjectNullEquality() {
    auto modules = compile({{"equality.rs", R"(
module Game.Equality;
class Item { int value; }
bool isNull(Item item) { return item == null; }
bool same(Item left, Item right) { return left == right; }
Item create() { return new Item(); }
)"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto nullResult = interpreter.invoke(
        "Game.Equality::isNull",
        {realscript::runtime::NullObject{}});
    require(nullResult.succeeded && std::get<bool>(nullResult.value),
        "object null equality failed");
    const auto created = interpreter.invoke("Game.Equality::create");
    require(created.succeeded, "object creation failed");
    const auto same = interpreter.invoke(
        "Game.Equality::same",
        {created.value, created.value});
    require(same.succeeded && std::get<bool>(same.value),
        "object identity equality failed");
}



void testCrossModuleObjectDescriptor() {
    auto modules = compile({
        {"model.rs", R"(
module Game.Model;
class Point { int x; }
Point create() { Point point = new Point(); point.x = 21; return point; }
)"},
        {"app.rs", R"(
module Game.App;
import Game.Model;
int read(Point point) { return point.x * 2; }
int main() { return read(create()); }
)"},
    });
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Game.App::main");
    require(result.succeeded && std::get<std::int64_t>(result.value) == 42,
        "cross-module object type descriptor or call failed");
}

void testRuntimeRejectsWrongObjectType() {
    auto modules = compile({{"types.rs", R"(
module Game.Types;
class A { int value; }
class B { int value; }
B makeB() { return new B(); }
A identity(A value) { return value; }
)"}});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto b = interpreter.invoke("Game.Types::makeB");
    require(b.succeeded, "B object creation failed");
    const auto result = interpreter.invoke("Game.Types::identity", {b.value});
    require(!result.succeeded, "B object was accepted as an A argument");
    require(result.error.code == realscript::runtime::ErrorCode::TypeMismatch,
        "wrong object argument produced the wrong runtime error");
}

void testObjectCodecRoundTrip() {
    auto modules = compile({{"point.rs", pointSource}});
    const auto encoded = realscript::bytecode::encodeModule(modules.front());
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(realscript::bytecode::decodeModule(encoded, decoded, diagnostics),
        "object module decode failed");
    require(!diagnostics.hasErrors(), "object module decode produced diagnostics");
    require(decoded.types.size() == 1 && decoded.types.front().fields.size() == 2,
        "type descriptors were not preserved by the codec");
    require(realscript::bytecode::encodeModule(decoded) == encoded,
        "object module codec is not canonical");
}

void testVerifierRejectsInvalidFieldDescriptor() {
    auto modules = compile({{"point.rs", pointSource}});
    auto module = modules.front();
    bool corrupted = false;
    for (auto& function : module.functions) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.opcode == realscript::bytecode::Opcode::LoadField) {
                    instruction.index = 99;
                    corrupted = true;
                    break;
                }
            }
        }
    }
    require(corrupted, "field verifier fixture did not find a field load");
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(!realscript::bytecode::verifyModule(module, diagnostics),
        "invalid field descriptor was accepted");
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };

    run("Type descriptor and object bytecode", testTypeDescriptorAndObjectBytecode);
    run("Object execution and defaults", testObjectExecutionAndFieldDefaults);
    run("Null reference trap", testNullReferenceTrap);
    run("Descriptor reference map", testObjectGraphUsesDescriptorReferenceMap);
    run("Object null equality", testObjectNullEquality);
    run("Cross-module object descriptor", testCrossModuleObjectDescriptor);
    run("Runtime exact object type", testRuntimeRejectsWrongObjectType);
    run("Object codec round trip", testObjectCodecRoundTrip);
    run("Verifier field descriptor", testVerifierRejectsInvalidFieldDescriptor);
    return failures == 0 ? 0 : 1;
}
