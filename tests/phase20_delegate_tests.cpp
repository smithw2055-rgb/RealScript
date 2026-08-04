#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/runtime/Runtime.h"

#include <iostream>
#include <stdexcept>
#include <string>

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

std::vector<realscript::bytecode::Module> compileModules(
    const std::string& source) {
    realscript::compiler::Compilation compilation({{
        "phase20-lifecycle.rs", source}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "Phase 20 lifecycle compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    std::vector<realscript::bytecode::Module> modules;
    for (const auto& module : build.modules) {
        modules.push_back(lowerer.lower(module));
    }
    return modules;
}

void testDelegateDescriptorAndArtifactRoundTrip() {
    realscript::compiler::Compilation compilation({{
        "phase20-descriptor.rs",
        R"(
module Phase20.Descriptor;

delegate int Transform(int value);

class Holder
{
    Transform current;
}

Transform Identity(Transform input)
{
    return input;
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "delegate descriptor compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    require(build.modules.size() == 1,
        "delegate descriptor module was not emitted");

    const realscript::semantic::TypeSymbol* descriptor = nullptr;
    const realscript::semantic::TypeSymbol* holder = nullptr;
    for (const auto& type : build.modules.front().types) {
        if (type.name == "Transform") descriptor = &type;
        if (type.name == "Holder") holder = &type;
    }
    require(descriptor && descriptor->delegateType && descriptor->sealedType,
        "delegate did not become an exact sealed type descriptor");
    require(descriptor->methods.size() == 1 &&
            descriptor->methods.front().name == "Invoke" &&
            descriptor->methods.front().returnType ==
                realscript::semantic::PrimitiveType::Int &&
            descriptor->methods.front().parameters.size() == 2 &&
            descriptor->methods.front().parameters[1].type ==
                realscript::semantic::PrimitiveType::Int,
        "delegate Invoke signature was not retained");
    require(holder && holder->fields.size() == 1 &&
            holder->fields.front().type ==
                realscript::semantic::PrimitiveType::Object &&
            holder->fields.front().typeName ==
                "Phase20.Descriptor::Transform",
        "delegate field lost exact named-type identity");

    realscript::bytecode::Lowerer lowerer;
    const auto bytecode = lowerer.lower(build.modules.front());
    const auto encoded = realscript::bytecode::encodeModule(bytecode);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag decodeDiagnostics;
    require(realscript::bytecode::decodeModule(
                encoded, decoded, decodeDiagnostics),
        "delegate artifact decode failed:\n" +
            diagnosticsText(decodeDiagnostics));
    const realscript::semantic::TypeSymbol* decodedDescriptor = nullptr;
    for (const auto& type : decoded.types) {
        if (type.name == "Transform") decodedDescriptor = &type;
    }
    require(decodedDescriptor && decodedDescriptor->delegateType &&
            decodedDescriptor->methods.size() == 1 &&
            decodedDescriptor->methods.front().name == "Invoke",
        "delegate descriptor was not preserved by .rsbc round trip");
    require(realscript::bytecode::encodeModule(decoded) == encoded,
        "delegate artifact round trip was not canonical");
}

void testStaticMethodReferenceInvocation() {
    realscript::compiler::Compilation compilation({{
        "phase20-static.rs",
        R"(
module Phase20.Static;

delegate int Transform(int value);

int Increment(int value)
{
    return value + 1;
}

int main()
{
    Transform transform = Increment;
    return transform(41);
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "static delegate compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::diagnostics::DiagnosticBag verifyDiagnostics;
    require(realscript::bytecode::verifyModule(
                module, verifyDiagnostics),
        "static delegate bytecode verification failed:\n" +
            diagnosticsText(verifyDiagnostics));
    const auto encoded = realscript::bytecode::encodeModule(module);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag decodeDiagnostics;
    require(realscript::bytecode::decodeModule(
                encoded, decoded, decodeDiagnostics),
        "static delegate artifact decode failed:\n" +
            diagnosticsText(decodeDiagnostics));
    require(realscript::bytecode::encodeModule(decoded) == encoded,
        "static delegate artifact round trip was not canonical");
    realscript::runtime::Interpreter interpreter({std::move(decoded)});
    const auto result = interpreter.invoke("Phase20.Static::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "static delegate invocation failed: " + result.error.message);
}

void testInstanceVirtualAndInterfaceMethodReferences() {
    realscript::compiler::Compilation compilation({{
        "phase20-dispatch.rs",
        R"(
module Phase20.Dispatch;

delegate int Reader();

interface IValue
{
    int Read();
}

class Base : IValue
{
    protected int value;
    public Base(int initial) { value = initial; }
    public virtual int Read() { return value; }
}

class Derived : Base
{
    public Derived(int initial) : base(initial) { }
    public override int Read() { return value + 2; }
}

Reader CreateVirtual(Base value)
{
    return value.Read;
}

Reader CreateInterface(IValue value)
{
    return value.Read;
}

int Invoke(Reader reader)
{
    return reader();
}

int main()
{
    Derived value = new Derived(40);
    Reader first = CreateVirtual(value);
    Reader second = CreateInterface(value);
    return Invoke(first) + second() - 42;
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "dispatch delegate compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::diagnostics::DiagnosticBag verifyDiagnostics;
    const auto verified = realscript::bytecode::verifyModule(
        module, verifyDiagnostics);
    require(verified,
        "dispatch delegate bytecode verification failed:\n" +
            diagnosticsText(verifyDiagnostics));
    realscript::runtime::Interpreter interpreter({std::move(module)});
    const auto result = interpreter.invoke("Phase20.Dispatch::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "virtual/interface delegate invocation failed: " +
            result.error.message);
}

void testHeapClosureCaptureByValue() {
    realscript::compiler::Compilation compilation({{
        "phase20-closure.rs",
        R"(
module Phase20.Closure;

delegate int Transform(int value);

Transform Create(int offset)
{
    Transform transform = value => value + offset;
    return transform;
}

int main()
{
    Transform transform = Create(2);
    return transform(40);
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "closure compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    bool foundClosure = false;
    bool foundCaptureCell = false;
    for (const auto& type : build.modules.front().types) {
        if (type.synthetic &&
            type.name.find("$closure_") == 0) {
            foundClosure = type.fields.size() == 1 &&
                type.fields.front().name == "offset" &&
                type.fields.front().type ==
                    realscript::semantic::PrimitiveType::Object;
        }
        if (type.synthetic && type.name.find("__RsRef__") == 0 &&
            type.fields.size() == 1 &&
            type.fields.front().name == "Value" &&
            type.fields.front().type ==
                realscript::semantic::PrimitiveType::Int) {
            foundCaptureCell = true;
        }
    }
    require(foundClosure && foundCaptureCell,
        "lambda capture did not produce precise closure/cell descriptors");
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::diagnostics::DiagnosticBag verifyDiagnostics;
    const auto verified = realscript::bytecode::verifyModule(
        module, verifyDiagnostics);
    require(verified,
        "closure bytecode verification failed:\n" +
            diagnosticsText(verifyDiagnostics));
    realscript::runtime::Interpreter interpreter({std::move(module)});
    const auto result = interpreter.invoke("Phase20.Closure::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "capturing closure invocation failed: " + result.error.message);
}

void testMutableCaptureSharedByDelegateCopies() {
    realscript::compiler::Compilation compilation({{
        "phase20-mutable-closure.rs",
        R"(
module Phase20.MutableClosure;

delegate int Reader();

int main()
{
    int count = 38;
    Reader first = () => count = count + 1;
    Reader copy = first;
    Reader second = () => count = count + 1;
    first();
    copy();
    count = count + 1;
    return second();
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "mutable closure compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::runtime::Interpreter interpreter({std::move(module)});
    const auto result = interpreter.invoke(
        "Phase20.MutableClosure::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "mutable capture was not shared by delegate copies and closures: " +
            result.error.message);
}

void testDelegateCombinationAndRemoval() {
    realscript::compiler::Compilation compilation({{
        "phase20-multicast.rs",
        R"(
module Phase20.Multicast;

delegate void Action();

class Counter
{
    int value;
    public void One() { value = value * 10 + 1; }
    public void Two() { value = value * 10 + 2; }
    public int Read() { return value; }
}

int main()
{
    Counter counter = new Counter();
    Action one = counter.One;
    Action two = counter.Two;
    Action both = one + two;
    both();
    Action remaining = both - two;
    remaining();
    return counter.Read();
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "multicast delegate compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::runtime::Interpreter interpreter({std::move(module)});
    const auto result = interpreter.invoke("Phase20.Multicast::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 121,
        "delegate combination/removal failed: " + result.error.message);
}

void testMutableParameterCapture() {
    realscript::compiler::Compilation compilation({{
        "phase20-parameter-capture.rs",
        R"(
module Phase20.ParameterCapture;
delegate int Reader();
int Run(int count)
{
    Reader first = () => count = count + 1;
    Reader second = () => count = count + 1;
    first();
    return second();
}
int main() { return Run(40); }
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "parameter capture compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::runtime::Interpreter interpreter({std::move(module)});
    const auto result = interpreter.invoke(
        "Phase20.ParameterCapture::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "mutable parameter capture was not shared: " +
            result.error.message);
}

void testDelegateBackedEventStorage() {
    realscript::compiler::Compilation compilation({{
        "phase20-events.rs",
        R"(
module Phase20.Events;

delegate void Changed(int value);

class Counter
{
    event Changed Updated;
    int total;

    public Counter()
    {
        Updated += Add;
        Updated += value => total = total + value;
    }

    void Add(int value) { total = total + value; }

    public int Run()
    {
        Updated(3);
        Updated -= Add;
        Updated(2);
        return total;
    }
}

int main() { return new Counter().Run(); }
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "delegate-backed event compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    const realscript::semantic::TypeSymbol* counter = nullptr;
    for (const auto& type : build.modules.front().types) {
        if (type.name == "Counter") counter = &type;
    }
    require(counter && counter->events.size() == 1 &&
            counter->events.front().handlers.empty() &&
            counter->events.front().subscriptions.empty() &&
            counter->events.front().storageField.typeName ==
                "Phase20.Events::Changed",
        "event retained Phase 18 subscription slots instead of delegate storage");
    std::size_t eventFields = 0;
    for (const auto& field : counter->fields) {
        if (field.name.find("$event_Updated_") == 0) ++eventFields;
    }
    require(eventFields == 1,
        "event did not emit exactly one delegate-valued storage field");
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::runtime::Interpreter interpreter({std::move(module)});
    const auto result = interpreter.invoke("Phase20.Events::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 8,
        "delegate-backed event execution failed: " + result.error.message);
}

void testClosureHotReloadPolicies() {
    const auto initial = compileModules(R"(
module Phase20.Lifecycle;
delegate int Reader();
Reader Create(int first, int second)
{
    return () => first + 1;
}
int main() { Reader reader = Create(40, 2); return reader(); }
)");
    realscript::runtime::RuntimeError error;
    auto linked = realscript::runtime::ProgramImage::link(initial, error);
    require(linked.has_value(),
        "Phase 20 lifecycle image failed to link: " + error.message);

    auto bodyOnly = compileModules(R"(
module Phase20.Lifecycle;
delegate int Reader();
Reader Create(int first, int second)
{

    return () => first + 2;
}
int main() { Reader reader = Create(40, 2); return reader(); }
)");
    const auto accepted = realscript::hot_reload::prepare(
        *linked, std::move(bodyOnly));
    require(accepted.compatible && !accepted.changedFunctions.empty(),
        "body-only lambda edit was rejected by hot reload");

    auto layoutChange = compileModules(R"(
module Phase20.Lifecycle;
delegate int Reader();
Reader Create(int first, int second)
{
    return () => first + second;
}
int main() { Reader reader = Create(40, 2); return reader(); }
)");
    const auto rejected = realscript::hot_reload::prepare(
        *linked, std::move(layoutChange));
    bool layoutIssue = false;
    for (const auto& issue : rejected.issues) {
        layoutIssue = layoutIssue || issue.kind ==
            realscript::hot_reload::ReloadIssueKind::TypeLayoutChanged;
    }
    require(!rejected.compatible && layoutIssue,
        "closure capture-layout change was accepted by hot reload");
}

void testSubscriberSideEventAccess() {
    realscript::compiler::Compilation compilation({{
        "phase20-event-access.rs",
        R"(
module Phase20.EventAccess;
delegate void Changed(int value);
class Publisher
{
    public event Changed Updated;
    public void Raise(int value) { Updated(value); }
}
class Subscriber
{
    int total;
    void Add(int value) { total = total + value; }
    public int Run()
    {
        Publisher publisher = new Publisher();
        publisher.Updated += Add;
        publisher.Raise(3);
        publisher.Updated -= Add;
        publisher.Raise(4);
        return total;
    }
}
int main() { Subscriber value = new Subscriber(); return value.Run(); }
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "subscriber-side event compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::runtime::Interpreter interpreter({std::move(module)});
    const auto result = interpreter.invoke("Phase20.EventAccess::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 3,
        "subscriber-side event add/remove failed: " +
            result.error.message);
}

void testDelegateReferenceParameters() {
    realscript::compiler::Compilation compilation({{
        "phase20-delegate-ref.rs",
        R"(
module Phase20.DelegateRef;
delegate void Mutator(ref int value);
delegate int Reader(in int value);
delegate void Initializer(out int value);
void AddTwo(ref int value) { value = value + 2; }
int Read(in int value) { return value; }
void Set(out int value) { value = 40; }
int main()
{
    int value;
    Initializer initialize = Set;
    Mutator mutate = AddTwo;
    Reader read = Read;
    initialize(out value);
    mutate(ref value);
    return read(in value);
}
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "reference delegate compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::diagnostics::DiagnosticBag diagnostics;
    require(realscript::bytecode::verifyModule(module, diagnostics),
        "reference delegate bytecode verification failed:\n" +
            diagnosticsText(diagnostics));
    const auto encoded = realscript::bytecode::encodeModule(module);
    realscript::bytecode::Module decoded;
    realscript::diagnostics::DiagnosticBag decodeDiagnostics;
    require(realscript::bytecode::decodeModule(
                encoded, decoded, decodeDiagnostics),
        "reference delegate artifact decode failed:\n" +
            diagnosticsText(decodeDiagnostics));
    realscript::runtime::Interpreter interpreter({std::move(decoded)});
    const auto result = interpreter.invoke("Phase20.DelegateRef::main");
    require(result.succeeded &&
            std::get<std::int64_t>(result.value) == 42,
        "reference delegate invocation failed: " +
            result.error.message);
}

void testDelegateHeapSnapshotRestore() {
    realscript::compiler::Compilation compilation({{
        "phase20-snapshot.rs",
        R"(
module Phase20.Snapshot;
delegate void Changed(int value);
class Holder
{
    event Changed Updated;
    int total;
    public Holder()
    {
        Updated += Add;
        Updated += value => total = total + value;
    }
    void Add(int value) { total = total + value; }
    public int Fire(int value) { Updated(value); return total; }
}
Holder main() { return new Holder(); }
)"}});
    const auto build = compilation.build();
    require(!build.diagnostics.hasErrors(),
        "delegate snapshot fixture compilation failed:\n" +
            diagnosticsText(build.diagnostics));
    realscript::bytecode::Lowerer lowerer;
    auto module = lowerer.lower(build.modules.front());
    realscript::runtime::Interpreter interpreter({std::move(module)});
    const auto created = interpreter.invoke("Phase20.Snapshot::main");
    require(created.succeeded &&
            std::holds_alternative<realscript::runtime::ObjectRef>(
                created.value),
        "delegate snapshot fixture did not return its holder");
    const auto holder = std::get<realscript::runtime::ObjectRef>(
        created.value);
    auto root = interpreter.heap()->retain(holder);
    const auto snapshot = interpreter.heap()->snapshot();
    require(snapshot.objects.size() >= 5 &&
            !snapshot.roots.empty(),
        "delegate snapshot omitted the invocation/capture graph");

    const auto mutated = interpreter.invoke(
        "Phase20.Snapshot::Holder.Fire",
        {holder, std::int64_t{3}});
    require(mutated.succeeded &&
            std::get<std::int64_t>(mutated.value) == 6,
        "delegate snapshot fixture mutation failed");
    realscript::runtime::RuntimeError restoreError;
    require(interpreter.heap()->restore(snapshot, &restoreError),
        "delegate heap snapshot restore failed: " +
            restoreError.message);
    const auto replayed = interpreter.invoke(
        "Phase20.Snapshot::Holder.Fire",
        {holder, std::int64_t{2}});
    require(replayed.succeeded &&
            std::get<std::int64_t>(replayed.value) == 4,
        "delegate invocation/capture state was not restored");
}

} // namespace

int main() {
    try {
        testDelegateDescriptorAndArtifactRoundTrip();
        testStaticMethodReferenceInvocation();
        testInstanceVirtualAndInterfaceMethodReferences();
        testHeapClosureCaptureByValue();
        testMutableCaptureSharedByDelegateCopies();
        testDelegateCombinationAndRemoval();
        testMutableParameterCapture();
        testDelegateBackedEventStorage();
        testClosureHotReloadPolicies();
        testSubscriberSideEventAccess();
        testDelegateReferenceParameters();
        testDelegateHeapSnapshotRestore();
        std::cout << "Phase 20 delegate tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
