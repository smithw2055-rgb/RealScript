#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
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
    require(!result.diagnostics.hasErrors(), "source compilation failed");
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

void testStringAndObjectLayouts() {
    realscript::runtime::ManagedHeap heap;
    auto text = heap.allocateString("hello");
    auto array = heap.allocateArray(2, std::int64_t{7});
    auto record = heap.allocateRecord(2);
    require(text && array && record, "managed allocations failed");
    require(heap.stringView(*text) == std::optional<std::string_view>{"hello"},
        "managed string payload is incorrect");
    require(heap.arrayLength(*array) == std::optional<std::size_t>{2},
        "managed array length is incorrect");
    require(std::get<std::int64_t>(*heap.arrayGet(*array, 1)) == 7,
        "managed array value is incorrect");
    require(heap.fieldSet(*record, 0, *text), "record field write failed");
    require(std::get<realscript::runtime::ObjectRef>(*heap.fieldGet(*record, 0)) == *text,
        "record field value is incorrect");
}

void testUnreachableObjectsAreReclaimed() {
    realscript::runtime::ManagedHeap heap;
    const auto text = heap.allocateString("temporary");
    require(text.has_value(), "managed string allocation failed");
    realscript::runtime::ShadowStack roots;
    heap.collectFull(roots);
    require(!heap.isAlive(*text), "unreachable object was not reclaimed");
    require(heap.statistics().objectsReclaimed == 1,
        "reclaimed object statistics are incorrect");
}

void testTransitiveGraphAndPreciseRoots() {
    realscript::runtime::ManagedHeap heap;
    const auto child = heap.allocateString("child");
    const auto parent = heap.allocateArray(1);
    require(child && parent, "managed graph allocation failed");
    require(heap.arraySet(*parent, 0, *child), "managed graph write failed");

    std::vector<realscript::runtime::Value> registers{*parent};
    realscript::runtime::ShadowStack roots;
    roots.pushFrame(nullptr, nullptr, &registers);
    heap.collectFull(roots);
    require(heap.isAlive(*parent), "root object was reclaimed");
    require(heap.isAlive(*child), "transitively reachable object was reclaimed");

    registers[0] = std::monostate{};
    heap.collectFull(roots);
    require(!heap.isAlive(*parent), "cleared parent root was retained");
    require(!heap.isAlive(*child), "orphaned child was retained");
    roots.popFrame();
}

void testPersistentRootsAndGenerationSafety() {
    realscript::runtime::ManagedHeap heap;
    const auto first = heap.allocateString("rooted");
    require(first.has_value(), "managed allocation failed");
    const auto token = heap.addPersistentRoot(*first);
    require(token != 0, "persistent root registration failed");
    realscript::runtime::ShadowStack roots;
    heap.collectFull(roots);
    require(heap.isAlive(*first), "persistent root was reclaimed");
    require(heap.removePersistentRoot(token), "persistent root removal failed");
    heap.collectFull(roots);
    require(!heap.isAlive(*first), "removed persistent root was retained");

    const auto second = heap.allocateString("reused");
    require(second.has_value(), "slot reuse allocation failed");
    require(second->slot == first->slot, "free slot was not reused");
    require(second->generation != first->generation,
        "reused slot did not advance its generation");
    require(!heap.isAlive(*first), "stale object handle became valid again");
}

void testIncrementalBudgetAndWriteBarrier() {
    realscript::runtime::ManagedHeap heap;
    const auto owner = heap.allocateRecord(1);
    const auto child = heap.allocateString("late-child");
    require(owner && child, "write-barrier graph allocation failed");

    std::vector<realscript::runtime::Value> registers{*owner};
    realscript::runtime::ShadowStack roots;
    roots.pushFrame(nullptr, nullptr, &registers);
    heap.requestCollection();

    require(heap.step(roots, 1) <= 1, "mark step exceeded the work budget");
    while (heap.phase() == realscript::runtime::GcPhase::Mark) {
        require(heap.step(roots, 1) <= 1, "mark phase exceeded the work budget");
    }
    require(heap.phase() == realscript::runtime::GcPhase::Sweep,
        "collector did not enter sweep phase");

    require(heap.fieldSet(*owner, 0, *child), "write barrier field write failed");
    require(heap.phase() == realscript::runtime::GcPhase::Mark,
        "write barrier did not reopen marking");
    while (heap.phase() != realscript::runtime::GcPhase::Idle ||
           heap.collectionRequested()) {
        require(heap.step(roots, 1) <= 1, "incremental step exceeded the work budget");
    }
    require(heap.isAlive(*owner), "write-barrier owner was reclaimed");
    require(heap.isAlive(*child), "write-barrier child was reclaimed");
    roots.popFrame();
}

void testIncrementalSweepProgressDuringAllocation() {
    realscript::runtime::ManagedHeap heap;
    for (int index = 0; index < 128; ++index) {
        require(heap.allocateString("garbage").has_value(),
            "sweep-progress setup allocation failed");
    }
    realscript::runtime::ShadowStack roots;
    heap.requestCollection();
    while (heap.phase() == realscript::runtime::GcPhase::Idle ||
           heap.phase() == realscript::runtime::GcPhase::Mark) {
        require(heap.step(roots, 1) <= 1,
            "sweep-progress mark exceeded the work budget");
    }

    std::vector<realscript::runtime::ObjectRef> allocatedDuringSweep;
    std::size_t steps = 0;
    while (heap.phase() != realscript::runtime::GcPhase::Idle ||
           heap.collectionRequested()) {
        if (heap.phase() == realscript::runtime::GcPhase::Sweep) {
            const auto value = heap.allocateString("during-sweep");
            require(value.has_value(), "allocation during sweep failed");
            allocatedDuringSweep.push_back(*value);
        }
        require(heap.step(roots, 1) <= 1,
            "sweep-progress step exceeded the work budget");
        require(++steps < 1024,
            "continuous allocation prevented incremental sweep completion");
    }
    require(!allocatedDuringSweep.empty(),
        "sweep-progress test did not allocate during sweep");
    for (const auto value : allocatedDuringSweep) {
        require(heap.isAlive(value),
            "an object allocated during sweep was reclaimed in its birth cycle");
    }
}

void testInterpreterShadowStackRootsManagedValues() {
    auto modules = compile({{
        "identity.rs",
        "module Demo; string identity(string value) { int n = 8; while (n > 0) n = n - 1; return value; }",
    }});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto managed = interpreter.heap()->allocateString("managed-value");
    require(managed.has_value(), "managed argument allocation failed");
    interpreter.heap()->requestCollection();

    realscript::runtime::ExecutionOptions options;
    options.limits.gcWorkBudget = 1;
    const auto result = interpreter.invoke("Demo::identity", {*managed}, options);
    require(result.succeeded, "managed string interpreter execution failed");
    require(std::get<realscript::runtime::ObjectRef>(result.value) == *managed,
        "managed argument identity changed");
    require(interpreter.heap()->isAlive(*managed),
        "interpreter shadow stack did not retain the managed argument");
    require(realscript::runtime::valueToString(result.value, interpreter.heap().get()) ==
            "managed-value",
        "managed result string could not be resolved");
    require(result.statistics.gcWorkPerformed > 0,
        "interpreter did not perform incremental GC work");
}


void testStringLiteralUsesManagedHeap() {
    auto modules = compile({{
        "greeting.rs",
        "module Demo; string greeting() { return \"hello managed\"; }",
    }});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto result = interpreter.invoke("Demo::greeting");
    require(result.succeeded, "managed string literal execution failed");
    require(std::holds_alternative<realscript::runtime::ObjectRef>(result.value),
        "string literal did not produce a managed object reference");
    const auto reference = std::get<realscript::runtime::ObjectRef>(result.value);
    require(reference.kind == realscript::runtime::ObjectKind::String,
        "string literal produced the wrong object kind");
    require(interpreter.heap()->stringView(reference) ==
            std::optional<std::string_view>{"hello managed"},
        "managed string literal payload is incorrect");
}



void testManagedAndHostStringEquality() {
    auto modules = compile({{
        "equal.rs",
        "module Demo; bool equal(string left, string right) { return left == right; }",
    }});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto managed = interpreter.heap()->allocateString("same");
    require(managed.has_value(), "managed equality fixture allocation failed");
    const auto result = interpreter.invoke(
        "Demo::equal",
        {*managed, std::string{"same"}});
    require(result.succeeded && std::get<bool>(result.value),
        "managed and host strings were not compared by content");
}

void testStaleManagedArgumentIsRejected() {
    auto modules = compile({{
        "stale.rs",
        "module Demo; string identity(string value) { return value; }",
    }});
    realscript::runtime::Interpreter interpreter(std::move(modules));
    const auto reference = interpreter.heap()->allocateString("stale");
    require(reference.has_value(), "stale-reference allocation failed");
    realscript::runtime::ShadowStack roots;
    interpreter.heap()->collectFull(roots);
    require(!interpreter.heap()->isAlive(*reference),
        "stale-reference fixture was not reclaimed");
    const auto result = interpreter.invoke("Demo::identity", {*reference});
    require(!result.succeeded, "stale managed argument was accepted");
    require(result.error.code ==
            realscript::runtime::ErrorCode::InvalidObjectReference,
        "stale managed argument produced the wrong error");
}

void testHeapLimitAndStressCollection() {
    realscript::runtime::HeapConfig config;
    config.initialCollectionThresholdBytes = 256;
    config.maximumHeapBytes = 16 * 1024;
    realscript::runtime::ManagedHeap heap(config);
    std::vector<realscript::runtime::Value> rootsStorage;
    rootsStorage.reserve(32);
    for (int index = 0; index < 32; ++index) {
        const auto value = heap.allocateArray(4, std::int64_t{index});
        require(value.has_value(), "stress allocation failed");
        if ((index % 4) == 0) rootsStorage.push_back(*value);
    }
    realscript::runtime::ShadowStack roots;
    roots.pushFrame(nullptr, nullptr, &rootsStorage);
    heap.collectFull(roots);
    require(heap.liveObjects() == rootsStorage.size(),
        "stress collection retained an incorrect object count");
    require(heap.statistics().collectionsCompleted >= 1,
        "stress collection did not complete");
    require(heap.statistics().peakLiveBytes >= heap.liveBytes(),
        "peak-live-byte statistics are invalid");
    roots.popFrame();
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

    run("String and object layouts", testStringAndObjectLayouts);
    run("Unreachable objects reclaimed", testUnreachableObjectsAreReclaimed);
    run("Transitive graph precise roots", testTransitiveGraphAndPreciseRoots);
    run("Persistent roots generation safety", testPersistentRootsAndGenerationSafety);
    run("Incremental budget write barrier", testIncrementalBudgetAndWriteBarrier);
    run("Incremental sweep allocation progress",
        testIncrementalSweepProgressDuringAllocation);
    run("Interpreter shadow-stack roots", testInterpreterShadowStackRootsManagedValues);
    run("Managed string literals", testStringLiteralUsesManagedHeap);
    run("Managed and host string equality", testManagedAndHostStringEquality);
    run("Stale managed arguments", testStaleManagedArgumentIsRejected);
    run("Heap limit stress collection", testHeapLimitAndStressCollection);
    return failures == 0 ? 0 : 1;
}
