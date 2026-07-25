#include "realscript/bytecode/Bytecode.h"
#include "realscript/runtime/Runtime.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void testStringAndStaleReference() {
    runtime::ManagedHeap heap;
    const auto string = heap.allocateString("managed");
    require(heap.alive(string), "allocated string must be alive");
    require(heap.stringValue(string) == std::optional<std::string>{"managed"},
        "managed string payload mismatch");
    heap.collect();
    require(!heap.alive(string), "unrooted string must be reclaimed");
    const auto replacement = heap.allocateString("replacement");
    require(replacement.slot == string.slot, "free slot should be reused");
    require(replacement.generation != string.generation, "generation must change after reuse");
    require(!heap.alive(string), "stale reference must remain invalid");
}

void testExactRootScope() {
    runtime::ManagedHeap heap;
    runtime::Value root = heap.allocateRecord(1);
    const auto reference = std::get<runtime::ObjectRef>(root);
    {
        runtime::ManagedHeap::RootScope roots(heap);
        roots.add(root);
        heap.collect();
        require(heap.alive(reference), "registered root must survive collection");
    }
    heap.collect();
    require(!heap.alive(reference), "object must die after root scope leaves");
}

void testArrayTransitiveMarking() {
    runtime::ManagedHeap heap;
    const auto child = heap.allocateString("child");
    const auto array = heap.allocateArray(2);
    require(heap.arraySet(array, 1, child), "array write failed");
    runtime::Value root = array;
    runtime::ManagedHeap::RootScope roots(heap);
    roots.add(root);
    heap.collect();
    require(heap.alive(array), "root array must survive");
    require(heap.alive(child), "array child must be marked transitively");
    const auto value = heap.arrayGet(array, 1);
    require(value && std::get<runtime::ObjectRef>(*value) == child,
        "array child payload mismatch");
}

void testIncrementalBudgetAndWriteBarrier() {
    runtime::ManagedHeap heap;
    const auto parent = heap.allocateRecord(1);
    const auto child = heap.allocateString("child");
    runtime::Value root = parent;
    runtime::ManagedHeap::RootScope roots(heap);
    roots.add(root);

    heap.requestCollection();
    require(heap.collectionInProgress(), "collection must begin");
    require(heap.step(1), "one step should leave collection active");
    require(heap.statistics().lastStepWork <= 1, "step exceeded work budget");
    require(heap.fieldSet(parent, 0, child), "record write failed");
    while (heap.collectionInProgress()) {
        (void)heap.step(1);
        require(heap.statistics().lastStepWork <= 1, "incremental step exceeded budget");
    }
    require(heap.alive(child), "write barrier must preserve newly linked white child");
    require(heap.statistics().incrementalSteps > 1, "collection should require multiple steps");
}

bytecode::Module makeRootModule() {
    bytecode::Module module;
    module.name = "Gc";

    bytecode::FunctionReference make;
    make.symbolId = 1001;
    make.name = "Host::make";
    make.returnType = semantic::PrimitiveType::Object;
    module.functionReferences.push_back(make);

    bytecode::FunctionReference collect;
    collect.symbolId = 1002;
    collect.name = "Host::collect";
    collect.returnType = semantic::PrimitiveType::Int;
    module.functionReferences.push_back(collect);

    bytecode::FunctionReference alive;
    alive.symbolId = 1003;
    alive.name = "Host::alive";
    alive.returnType = semantic::PrimitiveType::Bool;
    alive.parameterTypes = {semantic::PrimitiveType::Object};
    module.functionReferences.push_back(alive);

    bytecode::Function function;
    function.symbolId = 1;
    function.name = "main";
    function.returnType = semantic::PrimitiveType::Bool;
    function.localTypes = {semantic::PrimitiveType::Object};
    function.registerTypes = {
        semantic::PrimitiveType::Object,
        semantic::PrimitiveType::Int,
        semantic::PrimitiveType::Object,
        semantic::PrimitiveType::Bool,
    };

    bytecode::BasicBlock block;
    block.id = 0;
    bytecode::Instruction makeCall;
    makeCall.opcode = bytecode::Opcode::Call;
    makeCall.result = 0;
    makeCall.index = 0;
    block.instructions.push_back(makeCall);

    bytecode::Instruction store;
    store.opcode = bytecode::Opcode::StoreLocal;
    store.result = bytecode::InvalidRegister;
    store.index = 0;
    store.operands = {0};
    block.instructions.push_back(store);

    bytecode::Instruction collectCall;
    collectCall.opcode = bytecode::Opcode::Call;
    collectCall.result = 1;
    collectCall.index = 1;
    block.instructions.push_back(collectCall);

    bytecode::Instruction load;
    load.opcode = bytecode::Opcode::LoadLocal;
    load.result = 2;
    load.index = 0;
    block.instructions.push_back(load);

    bytecode::Instruction aliveCall;
    aliveCall.opcode = bytecode::Opcode::Call;
    aliveCall.result = 3;
    aliveCall.index = 2;
    aliveCall.operands = {2};
    block.instructions.push_back(aliveCall);
    block.terminator.kind = bytecode::TerminatorKind::ReturnValue;
    block.terminator.value = 3;
    function.blocks.push_back(block);
    module.functions.push_back(function);
    return module;
}

void testInterpreterRegisterAndLocalRoots() {
    auto module = makeRootModule();
    diagnostics::DiagnosticBag diagnostics;
    require(bytecode::verifyModule(module, diagnostics), "object bytecode must verify");

    runtime::Interpreter interpreter({module});
    const auto heap = interpreter.managedHeap();
    runtime::ObjectRef allocated;
    interpreter.setExternalResolver([heap, &allocated](
        const bytecode::FunctionReference& reference,
        const std::vector<runtime::Value>& arguments,
        runtime::RuntimeError&) -> std::optional<runtime::Value> {
        if (reference.symbolId == 1001) {
            allocated = heap->allocateRecord(2);
            return allocated;
        }
        if (reference.symbolId == 1002) {
            heap->collect();
            return static_cast<std::int64_t>(heap->statistics().liveObjects);
        }
        if (reference.symbolId == 1003) {
            return heap->alive(std::get<runtime::ObjectRef>(arguments.front()));
        }
        return std::nullopt;
    });

    const auto result = interpreter.invoke(semantic::SymbolId{1});
    require(result.succeeded && std::get<bool>(result.value),
        "object stored in interpreter local must survive host-triggered GC");
    require(heap->alive(allocated), "object must still be alive before frame roots leave");
    heap->collect();
    require(!heap->alive(allocated), "object must be reclaimed after invocation frame leaves");
}


void testRootMutationDuringSweep() {
    runtime::ManagedHeap heap;
    for (int index = 0; index < 8; ++index) (void)heap.allocateRecord(0);
    const auto survivor = heap.allocateRecord(0);
    runtime::Value root;
    runtime::ManagedHeap::RootScope roots(heap);
    roots.add(root);
    heap.requestCollection();
    (void)heap.step(1);
    root = survivor;
    while (heap.collectionInProgress()) (void)heap.step(1);
    require(heap.alive(survivor), "root mutation during collection must be observed");
}


void testCyclesAndPressure() {
    runtime::ManagedHeap heap;
    const auto left = heap.allocateRecord(1);
    const auto right = heap.allocateRecord(1);
    require(heap.fieldSet(left, 0, right), "left cycle write failed");
    require(heap.fieldSet(right, 0, left), "right cycle write failed");
    for (int index = 0; index < 1000; ++index) {
        (void)heap.allocateArray(4, std::int64_t{index});
    }
    heap.collect();
    require(!heap.alive(left) && !heap.alive(right),
        "unrooted object cycle must be reclaimed");
    require(heap.statistics().liveObjects == 0,
        "pressure collection must reclaim all unrooted objects");
    require(heap.statistics().reclaimedObjects >= 1002,
        "pressure reclaim statistics are incomplete");
}

void testHeapStatistics() {
    runtime::ManagedHeap heap;
    runtime::Value root = heap.allocateArray(4);
    {
        runtime::ManagedHeap::RootScope roots(heap);
        roots.add(root);
        heap.collect();
        require(heap.statistics().liveObjects == 1, "live object statistic mismatch");
        require(heap.statistics().peakBytes >= heap.statistics().liveBytes,
            "peak bytes must cover live bytes");
    }
    heap.collect();
    require(heap.statistics().liveObjects == 0, "reclaimed live count mismatch");
    require(heap.statistics().reclaimedObjects >= 1, "reclaimed statistic missing");
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    };
    run("String and stale reference", testStringAndStaleReference);
    run("Exact root scope", testExactRootScope);
    run("Array transitive marking", testArrayTransitiveMarking);
    run("Incremental budget and barrier", testIncrementalBudgetAndWriteBarrier);
    run("Interpreter register/local roots", testInterpreterRegisterAndLocalRoots);
    run("Root mutation during sweep", testRootMutationDuringSweep);
    run("Cycles and pressure", testCyclesAndPressure);
    run("Heap statistics", testHeapStatistics);
    return failures == 0 ? 0 : 1;
}
