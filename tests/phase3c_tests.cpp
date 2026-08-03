#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
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
        if (!realscript::bytecode::verifyModule(module, diagnostics)) {
            std::string message = "bytecode verification failed";
            for (const auto& diagnostic : diagnostics.items()) {
                message += "\n" + diagnostic.code + ": " + diagnostic.message;
            }
            throw std::runtime_error(message);
        }
        modules.push_back(std::move(module));
    }
    return modules;
}

const char* arraySource = R"(
module Game.Arrays;

class Item { int value; }
class Other { int value; }
class Box { int[] values; }

int numbers()
{
    int[] values = new int[3];
    values[0] = 40;
    values[1] = 2;
    return values[0] + values[1] + values.length;
}

bool strings()
{
    string[] values = new string[2];
    return values[0] == null && values.length == 2;
}

int objects()
{
    Item[] items = new Item[1];
    Item item = new Item();
    item.value = 7;
    items[0] = item;
    return items[0].value;
}

bool arrayFieldDefaultsToNull()
{
    Box box = new Box();
    return box.values == null;
}

int itemCount(Item[] items)
{
    return items.length;
}

Item[] makeGraph()
{
    Item[] items = new Item[1];
    Item item = new Item();
    item.value = 9;
    items[0] = item;
    return items;
}

int outOfBounds()
{
    int[] values = new int[1];
    return values[1];
}

int negativeLength()
{
    int[] values = new int[-1];
    return values.length;
}

int nullLength()
{
    int[] values = null;
    return values.length;
}
)";

void testLanguageArrays() {
    auto modules = compile({{"arrays.rs", arraySource}});
    require(modules.size() == 1, "expected one array module");
    const auto& module = modules.front();
    require(module.version.major == 0 && module.version.minor == 6,
        "Phase 3C bytecode must use format 0.6");
    const auto text = realscript::bytecode::disassembleModule(module);
    require(text.find("new.array int") != std::string::npos,
        "array allocation bytecode is missing");
    require(text.find("load.element") != std::string::npos,
        "array element load bytecode is missing");
    require(text.find("store.element") != std::string::npos,
        "array element store bytecode is missing");
    require(text.find("array.length") != std::string::npos,
        "array length bytecode is missing");

    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto numbers = interpreter.invoke("Game.Arrays::numbers");
    const auto strings = interpreter.invoke("Game.Arrays::strings");
    const auto objects = interpreter.invoke("Game.Arrays::objects");
    const auto fieldDefault = interpreter.invoke(
        "Game.Arrays::arrayFieldDefaultsToNull");
    require(numbers.succeeded &&
            std::get<std::int64_t>(numbers.value) == 45,
        "integer arrays returned the wrong result");
    require(strings.succeeded && std::get<bool>(strings.value),
        "string array defaults or length are incorrect");
    require(objects.succeeded &&
            std::get<std::int64_t>(objects.value) == 7,
        "object array load/store returned the wrong result");
    require(fieldDefault.succeeded && std::get<bool>(fieldDefault.value),
        "array fields must default to null");

    const auto otherType = realscript::semantic::stableTypeId(
        "Game.Arrays::Other");
    const auto otherArrayType = realscript::semantic::stableTypeId(
        "Game.Arrays::Other[]");
    const auto wrongArray = interpreter.heap()->allocateTypedArray(
        otherArrayType,
        realscript::semantic::PrimitiveType::Object,
        otherType,
        1,
        realscript::runtime::NullObject{});
    require(wrongArray.has_value(), "wrong-type array fixture allocation failed");
    const auto mismatch = interpreter.invoke(
        "Game.Arrays::itemCount", {*wrongArray});
    require(!mismatch.succeeded &&
            mismatch.error.code == realscript::runtime::ErrorCode::TypeMismatch,
        "runtime accepted an array with the wrong exact TypeId");
}

void testArrayRuntimeErrors() {
    realscript::runtime::Interpreter interpreter(
        compile({{"arrays.rs", arraySource}}));
    const auto bounds = interpreter.invoke("Game.Arrays::outOfBounds");
    const auto negative = interpreter.invoke("Game.Arrays::negativeLength");
    const auto nullLength = interpreter.invoke("Game.Arrays::nullLength");
    require(!bounds.succeeded &&
            bounds.error.code == realscript::runtime::ErrorCode::IndexOutOfRange,
        "out-of-range array access produced the wrong error");
    require(!negative.succeeded &&
            negative.error.code == realscript::runtime::ErrorCode::IndexOutOfRange,
        "negative array length produced the wrong error");
    require(!nullLength.succeeded &&
            nullLength.error.code == realscript::runtime::ErrorCode::NullReference,
        "null array length produced the wrong error");
}

void testArrayGcGraph() {
    realscript::runtime::Interpreter interpreter(
        compile({{"arrays.rs", arraySource}}));
    const auto result = interpreter.invoke("Game.Arrays::makeGraph");
    require(result.succeeded &&
            std::holds_alternative<realscript::runtime::ObjectRef>(result.value),
        "array graph function did not return a managed array");
    const auto array = std::get<realscript::runtime::ObjectRef>(result.value);
    const auto childValue = interpreter.heap()->arrayGet(array, 0);
    require(childValue &&
            std::holds_alternative<realscript::runtime::ObjectRef>(*childValue),
        "object array did not store its child");
    const auto child = std::get<realscript::runtime::ObjectRef>(*childValue);

    auto root = interpreter.heap()->retain(array);
    require(root.valid(), "RAII persistent root registration failed");
    realscript::runtime::ShadowStack roots;
    interpreter.heap()->collectFull(roots);
    require(interpreter.heap()->isAlive(array), "rooted array was reclaimed");
    require(interpreter.heap()->isAlive(child),
        "object array did not retain its child during GC");

    const auto path = interpreter.heap()->retainingPath(child);
    require(path.size() == 2 && path.front() == array && path.back() == child,
        "retaining path did not identify array ownership");
    const auto snapshot = interpreter.heap()->snapshot();
    require(snapshot.objects.size() == 2 && snapshot.roots.size() == 1,
        "heap snapshot did not include the graph and root");
    require(snapshot.toText().find("element[0]") != std::string::npos,
        "heap snapshot did not include the array edge");

    root.reset();
    interpreter.heap()->collectFull(roots);
    require(!interpreter.heap()->isAlive(array) &&
            !interpreter.heap()->isAlive(child),
        "unrooted array graph was not reclaimed");
}

void testArrayCodecAndVerifier() {
    auto module = compile({{"arrays.rs", arraySource}}).front();
    const auto encoded = realscript::bytecode::encodeModule(module);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag decodeDiagnostics;
    require(realscript::bytecode::decodeModule(
            encoded, decoded, decodeDiagnostics),
        "Phase 3C bytecode failed to decode");
    require(encoded == realscript::bytecode::encodeModule(decoded),
        "Phase 3C bytecode round trip is not canonical");

    bool corrupted = false;
    for (auto& function : decoded.functions) {
        for (auto& block : function.blocks) {
            for (auto& instruction : block.instructions) {
                if (instruction.opcode == realscript::bytecode::Opcode::NewArray &&
                    instruction.elementType ==
                        realscript::semantic::PrimitiveType::Int) {
                    instruction.elementTypeId = 123;
                    corrupted = true;
                    break;
                }
            }
            if (corrupted) break;
        }
        if (corrupted) break;
    }
    require(corrupted, "array fixture did not contain new.array");
    realscript::diagnostics::DiagnosticBag verifyDiagnostics;
    require(!realscript::bytecode::verifyModule(decoded, verifyDiagnostics),
        "verifier accepted corrupt array element metadata");

    auto corruptLocal = module;
    bool clearedArrayLocal = false;
    for (auto& function : corruptLocal.functions) {
        for (std::size_t index = 0; index < function.localTypes.size(); ++index) {
            if (function.localTypes[index] ==
                    realscript::semantic::PrimitiveType::Array &&
                index < function.localTypeIds.size()) {
                function.localTypeIds[index] = 0;
                clearedArrayLocal = true;
                break;
            }
        }
        if (clearedArrayLocal) break;
    }
    require(clearedArrayLocal, "array fixture did not contain an array local");
    realscript::diagnostics::DiagnosticBag localDiagnostics;
    require(!realscript::bytecode::verifyModule(
            corruptLocal, localDiagnostics),
        "verifier accepted an array local without an exact TypeId");

    auto corruptRegisterTable = module;
    bool removedRegisterTable = false;
    for (auto& function : corruptRegisterTable.functions) {
        if (!function.registerTypes.empty()) {
            function.registerTypeIds.clear();
            removedRegisterTable = true;
            break;
        }
    }
    require(removedRegisterTable,
        "array fixture did not contain a register table");
    realscript::diagnostics::DiagnosticBag registerDiagnostics;
    require(!realscript::bytecode::verifyModule(
            corruptRegisterTable, registerDiagnostics),
        "verifier accepted an omitted register TypeId table");
}

void testNativeHandleRegistry() {
    using namespace realscript;
    auto registry = std::make_shared<runtime::NativeHandleRegistry>();
    const auto textureType = semantic::stableTypeId("Host::Texture");
    const auto audioType = semantic::stableTypeId("Host::Audio");
    auto resource = std::make_shared<int>(42);
    const auto handle = registry->create(textureType, resource, "albedo");
    require(handle.valid() && registry->isAlive(handle),
        "native handle creation failed");
    auto resolved = registry->resolve(handle, textureType);
    require(resolved && *std::static_pointer_cast<int>(resolved) == 42,
        "native handle resolution failed");
    require(registry->debugName(handle) == "albedo",
        "native handle debug name was lost");
    require(handle.registryId == registry->registryId(),
        "native handle did not record its registry ownership identity");
    runtime::NativeHandleRegistry foreignRegistry;
    runtime::RuntimeError foreignHandle;
    require(!foreignRegistry.resolve(handle, textureType, &foreignHandle) &&
            foreignHandle.code == runtime::ErrorCode::InvalidNativeHandle,
        "native handle was accepted by a foreign registry");

    runtime::RuntimeError wrongType;
    require(!registry->resolve(handle, audioType, &wrongType) &&
            wrongType.code == runtime::ErrorCode::TypeMismatch,
        "native handle type mismatch was not rejected");
    require(registry->release(handle), "native handle release failed");
    runtime::RuntimeError stale;
    require(!registry->resolve(handle, textureType, &stale) &&
            stale.code == runtime::ErrorCode::InvalidNativeHandle,
        "stale native handle was not rejected");

    const auto replacement = registry->create(
        textureType, std::make_shared<int>(7), "replacement");
    require(replacement.slot == handle.slot &&
            replacement.generation != handle.generation,
        "native handle slot reuse did not advance the generation");
    require(registry->liveCount() == 1, "native handle live count is incorrect");

    const auto handleModules = compile({{
        "handles.rs",
        R"(
module Game.Handles;
handle echo(handle value) { return value; }
handle throughArray(handle value)
{
    handle[] values = new handle[1];
    values[0] = value;
    return values[0];
}
bool equal(handle left, handle right) { return left == right; }
)"}});
    runtime::RuntimeError linkError;
    auto image = runtime::ProgramImage::link(handleModules, linkError);
    require(image.has_value(), "native-handle program image failed to link");
    runtime::EngineRuntime engine(
        std::make_shared<runtime::ProgramImage>(std::move(*image)));
    engine.setNativeHandles(registry);
    require(engine.nativeHandles() == registry,
        "engine runtime did not retain the native-handle registry");

    const auto liveHandle = registry->create(
        textureType, std::make_shared<int>(99), "script-visible");
    const auto echoed = engine.invoke("Game.Handles::echo", {liveHandle});
    const auto throughArray = engine.invoke(
        "Game.Handles::throughArray", {liveHandle});
    const auto equal = engine.invoke(
        "Game.Handles::equal", {liveHandle, liveHandle});
    require(echoed.succeeded &&
            std::get<runtime::NativeHandle>(echoed.value) == liveHandle,
        "native handle did not round-trip through bytecode execution");
    require(throughArray.succeeded &&
            std::get<runtime::NativeHandle>(throughArray.value) == liveHandle,
        "native handle did not round-trip through a typed array");
    require(equal.succeeded && std::get<bool>(equal.value),
        "native handle identity equality failed");
    auto scriptResource = engine.nativeHandles()->resolve(
        std::get<runtime::NativeHandle>(echoed.value), textureType);
    require(scriptResource &&
            *std::static_pointer_cast<int>(scriptResource) == 99,
        "host could not validate a script-returned native handle");
    require(engine.nativeHandles()->release(liveHandle),
        "script-visible native handle release failed");
    registry->clear();
    require(registry->liveCount() == 0 && !registry->isAlive(replacement),
        "native handle clear did not invalidate resources");
}

void testHeapOwnershipAndDiagnostics() {
    using namespace realscript;
    runtime::ManagedHeap first;
    runtime::ManagedHeap second;
    const auto nodeType = semantic::stableTypeId("Diagnostics::Node");
    const auto arrayType = semantic::stableTypeId("Diagnostics::Node[]");
    const auto child = first.allocateObject(nodeType, {}, {});
    const auto array = first.allocateTypedArray(
        arrayType,
        semantic::PrimitiveType::Object,
        nodeType,
        1,
        runtime::NullObject{});
    require(child && array, "diagnostic graph allocation failed");
    require(first.arraySet(*array, 0, *child),
        "diagnostic graph edge creation failed");
    require(!second.isAlive(*child),
        "managed reference was accepted by a different heap");
    runtime::RuntimeError foreignError;
    require(!second.arraySet(*array, 0, *child, &foreignError) &&
            foreignError.code == runtime::ErrorCode::InvalidObjectReference,
        "cross-heap array mutation was not rejected");

    auto root = first.retain(*array);
    require(root.valid(), "persistent root did not retain the array");
    const auto summary = first.leakSummary();
    require(summary.find("2 live objects") != std::string::npos &&
            summary.find("1 persistent roots") != std::string::npos &&
            summary.find("array=1") != std::string::npos &&
            summary.find("record=1") != std::string::npos,
        "heap leak summary is incomplete");
    require(first.heapId() != second.heapId(),
        "managed heaps must have distinct ownership identities");
}

void testGcStress() {
    using namespace realscript;
    runtime::HeapConfig config;
    config.initialCollectionThresholdBytes = 1;
    config.maximumHeapBytes = 4 * 1024 * 1024;
    runtime::ManagedHeap heap(config);
    runtime::ShadowStack shadow;
    const auto nodeType = semantic::stableTypeId("Stress::Node");
    const auto arrayType = semantic::stableTypeId("Stress::Node[]");
    std::vector<runtime::PersistentRoot> roots;

    for (int iteration = 0; iteration < 256; ++iteration) {
        const auto child = heap.allocateObject(nodeType, {}, {});
        const auto array = heap.allocateTypedArray(
            arrayType,
            semantic::PrimitiveType::Object,
            nodeType,
            2,
            runtime::NullObject{});
        require(child && array, "GC stress allocation failed");
        require(heap.arraySet(*array, 0, *child),
            "GC stress edge creation failed");
        if (iteration % 31 == 0) roots.push_back(heap.retain(*array));
        (void)heap.step(shadow, 3);
    }
    heap.collectFull(shadow);
    require(heap.liveObjects() == roots.size() * 2,
        "incremental GC did not preserve exactly the rooted stress graphs: live=" +
            std::to_string(heap.liveObjects()) + ", expected=" +
            std::to_string(roots.size() * 2));
    roots.clear();
    heap.collectFull(shadow);
    require(heap.liveObjects() == 0,
        "GC stress graph leaked after roots were released");
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

    run("Language arrays", testLanguageArrays);
    run("Array runtime errors", testArrayRuntimeErrors);
    run("Array GC graph", testArrayGcGraph);
    run("Array codec and verifier", testArrayCodecAndVerifier);
    run("Native handle registry", testNativeHandleRegistry);
    run("Heap ownership and diagnostics", testHeapOwnershipAndDiagnostics);
    run("GC stress", testGcStress);
    return failures == 0 ? 0 : 1;
}
