#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/plugin/ModHost.h"
#include "realscript/reflection/Reflection.h"
#include "realscript/runtime/Runtime.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace realscript;

[[noreturn]] void failCheck(const char* expr, int line) noexcept {
    std::cerr << "Check failed at app_reflection_and_mod_host_tests.cpp:" << line << ": " << expr << std::endl;
    std::exit(1);
}

#define RS_CHECK(...)                                                                              \
    do {                                                                                           \
        if (!(__VA_ARGS__)) {                                                                      \
            failCheck(#__VA_ARGS__, __LINE__);                                                     \
        }                                                                                          \
    } while (false)

struct CompiledExtProgram {
    std::shared_ptr<runtime::ProgramImage> program;
    std::shared_ptr<runtime::ManagedHeap> heap;
    std::shared_ptr<runtime::BindingRegistry> bindings;
    std::shared_ptr<reflection::ReflectionRegistry> reflection;
};

CompiledExtProgram compileWithReflect(std::vector<compiler::SourceFile> sources) {
    sources.push_back(reflection::exportReflectDeclarationStub());
    compiler::Compilation compilation(std::move(sources));
    const auto buildResult = compilation.build();
    if (buildResult.diagnostics.hasErrors()) {
        for (const auto& d : buildResult.diagnostics.items()) {
            std::cerr << "Ext compile error [" << d.code << "]: " << d.message << "\n";
        }
    }
    RS_CHECK(!buildResult.diagnostics.hasErrors());

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> lowered;
    lowered.reserve(buildResult.modules.size());
    for (const auto& m : buildResult.modules) {
        lowered.push_back(lowerer.lower(m));
    }

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(lowered, linkErr);
    if (!linked) {
        std::cerr << "Ext link error: " << linkErr.message << "\n";
    }
    RS_CHECK(linked.has_value());

    CompiledExtProgram out;
    out.program = std::make_shared<runtime::ProgramImage>(std::move(*linked));
    out.heap = std::make_shared<runtime::ManagedHeap>();
    out.bindings = std::make_shared<runtime::BindingRegistry>();
    out.reflection = reflection::ReflectionRegistry::fromProgram(out.program, out.heap);
    reflection::registerReflectBindings(*out.bindings, out.reflection, out.heap, "reflection");
    return out;
}

void testReflectionMetadataAttributesAndDigest() {
    const std::string script = R"(module Ext.Meta;

[Category(name = "Gameplay", priority = 2)]
public class Hero {
    [Range(min = 0, max = 100)]
    public int hp = 80;

    [Serializable]
    public string title = "Knight";

    public Hero() {}

    [Command(alias = "heal")]
    public int heal(int amount) {
        hp = hp + amount;
        return hp;
    }
}
)";

    const auto built1 = compileWithReflect({{"meta.rscript", script}});
    const auto built2 = compileWithReflect({{"meta.rscript", script}});

    RS_CHECK(built1.reflection->metadataDigest() != 0);
    RS_CHECK(built1.reflection->metadataDigest() == built2.reflection->metadataDigest());

    const auto* heroType = built1.reflection->findType("Ext.Meta::Hero");
    RS_CHECK(heroType != nullptr);
    RS_CHECK(heroType->hasAttribute("Category"));
    const auto* catAttr = heroType->findAttribute("Category");
    RS_CHECK(catAttr != nullptr);

    const auto rangeFields = built1.reflection->findFieldsWithAttribute("Range");
    RS_CHECK(rangeFields.size() == 1);
    RS_CHECK(rangeFields[0].field != nullptr && rangeFields[0].field->name == "hp");

    const auto cmdMethods = built1.reflection->findMethodsWithAttribute("Command");
    RS_CHECK(cmdMethods.size() == 1);
    RS_CHECK(cmdMethods[0].method != nullptr && cmdMethods[0].method->name == "heal");

    const auto catTypes = built1.reflection->findTypesWithAttribute("Category");
    RS_CHECK(catTypes.size() == 1);

    std::cout << "[PASS] Reflection metadata, attributes & deterministic digest\n";
}

void testNonPublicEncapsulationEnforcement() {
    const std::string script = R"(module Ext.Security;
import RealScript.Reflect;

private class SecretVault {
    public int code = 42;
    public SecretVault() {}
}

public class Account {
    private int pin = 9999;
    public int balance = 100;

    [Secret(note = "internal")]
    private string token = "xyz";

    public Account() {}

    private int secretSum(int delta) { return pin + delta; }
    public int getPinPublic() { return pin; }
}

public int probeBlockedRead() {
    var acc = new Account();
    return Reflect.GetIntField(acc, "pin");
}

public bool probeBlockedSecretType() {
    return Reflect.HasType("Ext.Security::SecretVault");
}

public string probeBlockedAttr() {
    return Reflect.GetAttributeString("Ext.Security::Account.token", "Secret", "note");
}

public int probeAllowedRead() {
    var acc = new Account();
    return Reflect.GetIntField(acc, "balance");
}
)";

    auto built = compileWithReflect({{"security.rscript", script}});
    runtime::EngineRuntime rt(built.program);
    rt.setHeap(built.heap);
    rt.setBindings(built.bindings);

    runtime::ExecutionOptions capOpts;
    capOpts.enforceCapabilities = true;
    capOpts.grantedCapabilities.insert("reflection");

    runtime::ExecutionOptions deniedCapOpts;
    deniedCapOpts.enforceCapabilities = true;
    deniedCapOpts.grantedCapabilities.insert("ui");

    // Host default allowNonPublic=false MUST reject private types, private fields, private methods, private attrs.
    runtime::RuntimeError hostErr;
    const auto deniedObj = built.reflection->instantiateObject(
        rt, "Ext.Security::SecretVault", {}, hostErr);
    RS_CHECK(!deniedObj.has_value());

    const auto accObj = built.reflection->instantiateObject(
        rt, "Ext.Security::Account", {}, hostErr);
    RS_CHECK(accObj.has_value());

    const auto deniedField = built.reflection->getField(
        *built.heap, *accObj, "pin", hostErr);
    RS_CHECK(!deniedField.has_value());
    RS_CHECK(!built.reflection->setField(
        *built.heap, *accObj, "pin", runtime::Value{std::int64_t{1}}, hostErr));

    const auto deniedInvoke = built.reflection->invokeMethod(
        rt, *accObj, "secretSum", {runtime::Value{std::int64_t{1}}}, capOpts);
    RS_CHECK(!deniedInvoke.succeeded);

    // Explicit host override allowNonPublic=true grants host-level inspection & mutation.
    hostErr = {};
    const auto allowedSecret = built.reflection->instantiateObject(
        rt, "Ext.Security::SecretVault", {}, hostErr, true, capOpts, true);
    RS_CHECK(allowedSecret.has_value());

    const auto hostPin = built.reflection->getField(
        *built.heap, *accObj, "pin", hostErr, true);
    RS_CHECK(hostPin.has_value());
    RS_CHECK(std::get<std::int64_t>(*hostPin) == 9999);

    const auto hostMethodRes = built.reflection->invokeMethod(
        rt, *accObj, "secretSum", {runtime::Value{std::int64_t{1}}}, capOpts, true);
    RS_CHECK(hostMethodRes.succeeded);
    RS_CHECK(std::get<std::int64_t>(hostMethodRes.value) == 10000);

    // Script-side RealScript.Reflect must NEVER bypass encapsulation.
    const auto sBlockedField = rt.invoke("Ext.Security::probeBlockedRead", {}, capOpts);
    RS_CHECK(!sBlockedField.succeeded);

    const auto sBlockedType = rt.invoke("Ext.Security::probeBlockedSecretType", {}, capOpts);
    RS_CHECK(sBlockedType.succeeded);
    RS_CHECK(!std::get<bool>(sBlockedType.value));

    const auto sBlockedAttr = rt.invoke("Ext.Security::probeBlockedAttr", {}, capOpts);
    RS_CHECK(sBlockedAttr.succeeded);
    const bool attrIsEmptyOrNull =
        sBlockedAttr.value.rawTag() == runtime::ValueTag::NullString ||
        (std::holds_alternative<std::string>(sBlockedAttr.value) &&
         std::get<std::string>(sBlockedAttr.value).empty()) ||
        (std::holds_alternative<runtime::ObjectRef>(sBlockedAttr.value) &&
         built.heap->stringView(std::get<runtime::ObjectRef>(sBlockedAttr.value)) == "");
    RS_CHECK(attrIsEmptyOrNull);

    const auto sAllowedField = rt.invoke("Ext.Security::probeAllowedRead", {}, capOpts);
    RS_CHECK(sAllowedField.succeeded);
    RS_CHECK(std::get<std::int64_t>(sAllowedField.value) == 100);

    // Capability enforcement: without "reflection" granted, script-side RealScript.Reflect MUST fail.
    const auto sDeniedCap = rt.invoke("Ext.Security::probeAllowedRead", {}, deniedCapOpts);
    RS_CHECK(!sDeniedCap.succeeded);

    std::cout << "[PASS] Reflection non-public encapsulation enforcement\n";
}

void testFastViewSafetyPolymorphismAndStaticConstructors() {
    const std::string script = R"(module Ext.PolyStatic;

public class Trace {
    public static string log = "";
}

public class Animal {
    static Animal() {
        Trace.log = Trace.log + "Base>";
    }
    public int tag = 10;
    public Animal() {}
    public virtual int speak() { return 1; }
}

public class Wolf : Animal {
    static Wolf() {
        Trace.log = Trace.log + "Derived;";
    }
    public int fangs = 2;
    public Wolf() {}
    public override int speak() { return 99 + tag + fangs; }
}
)";

    auto built = compileWithReflect({{"poly_static.rscript", script}});
    runtime::EngineRuntime rt(built.program);
    rt.setHeap(built.heap);
    rt.setBindings(built.bindings);

    // Verify static initializer inheritance chain executes Base before Derived, once only,
    // triggered automatically on first instantiation of Derived.
    const auto* wolfType = built.reflection->findType("Ext.PolyStatic::Wolf");
    RS_CHECK(wolfType != nullptr);
    const auto* traceType = built.reflection->findType("Ext.PolyStatic::Trace");
    RS_CHECK(traceType != nullptr);
    runtime::RuntimeError err;

    const auto wolfObj = built.reflection->instantiateObject(
        rt, "Ext.PolyStatic::Wolf", {}, err);
    RS_CHECK(wolfObj.has_value());

    const auto traceLog = built.reflection->getStaticField(
        *built.heap, traceType->canonicalName, "log", err);
    RS_CHECK(traceLog.has_value() && std::holds_alternative<runtime::ObjectRef>(*traceLog));
    const auto logView = built.heap->stringView(std::get<runtime::ObjectRef>(*traceLog));
    RS_CHECK(logView.has_value() && *logView == "Base>Derived;");

    // Second instantiation does NOT re-run static constructors (idempotent cctor chain).
    const auto wolfObj2 = built.reflection->instantiateObject(
        rt, "Ext.PolyStatic::Wolf", {}, err);
    RS_CHECK(wolfObj2.has_value());
    const auto traceLog2 = built.reflection->getStaticField(
        *built.heap, traceType->canonicalName, "log", err);
    RS_CHECK(traceLog2.has_value() && std::holds_alternative<runtime::ObjectRef>(*traceLog2));
    const auto logView2 = built.heap->stringView(std::get<runtime::ObjectRef>(*traceLog2));
    RS_CHECK(logView2.has_value() && *logView2 == "Base>Derived;");

    // Verify ObjectFastView type & fieldCount safety, and polymorphic override invocation.

    runtime::ObjectFastView view{};
    RS_CHECK(built.heap->tryBindObjectFastView(*wolfObj, view));
    RS_CHECK(view.typeId == wolfType->typeId);
    RS_CHECK(view.fieldCount >= 2);

    const auto speakRes = built.reflection->invokeMethod(rt, *wolfObj, "speak");
    RS_CHECK(speakRes.succeeded);
    RS_CHECK(std::get<std::int64_t>(speakRes.value) == 111);

    // Invalid/stale ObjectRef safely fails ObjectFastView binding.
    runtime::ObjectFastView bogusView{};
    RS_CHECK(!built.heap->tryBindObjectFastView(
        runtime::ObjectRef{999999u, 1u, runtime::ObjectKind::Record, 0u}, bogusView));

    std::cout << "[PASS] ObjectFastView safety, polymorphic dispatch & static constructors\n";
}

void testSemVerAndKahnDagModPlanner() {
    const auto v123 = plugin::SemVer::parse("1.2.3");
    RS_CHECK(v123.has_value());
    RS_CHECK(v123->major == 1 && v123->minor == 2 && v123->patch == 3);

    const auto c1 = plugin::VersionConstraint::parse(">=1.2.0 <2.0.0");
    RS_CHECK(c1.has_value());
    RS_CHECK(c1->satisfies(*v123));
    RS_CHECK(!c1->satisfies(plugin::SemVer{2, 0, 0}));

    // Valid DAG: core <- gameplay <- hud
    std::vector<plugin::ModManifest> dag;
    plugin::ModManifest core{"mod.core", plugin::SemVer{1, 0, 0}, {}, {}, 4 * 1024 * 1024, 100000, ""};
    plugin::ModManifest gameplay{
        "mod.gameplay",
        plugin::SemVer{1, 2, 0},
        {{"mod.core", plugin::VersionConstraint::parse("^1.0.0").value()}},
        {}, 4 * 1024 * 1024, 100000, ""};
    plugin::ModManifest hud{
        "mod.hud",
        plugin::SemVer{1, 0, 1},
        {{"mod.gameplay", plugin::VersionConstraint::parse(">=1.1.0").value()}},
        {}, 4 * 1024 * 1024, 100000, ""};
    dag.push_back(hud);
    dag.push_back(core);
    dag.push_back(gameplay);

    const auto plan = plugin::resolveModLoadPlan(dag);
    RS_CHECK(plan.diagnostics.empty());
    RS_CHECK(plan.loadOrder.size() == 3);
    RS_CHECK(plan.loadOrder[0] == "mod.core");
    RS_CHECK(plan.loadOrder[1] == "mod.gameplay");
    RS_CHECK(plan.loadOrder[2] == "mod.hud");

    // Cycle detection test.
    plugin::ModManifest cycA{"mod.a", plugin::SemVer{1, 0, 0}, {{"mod.b", ">=1.0.0"}}, {}, 4 * 1024 * 1024, 100000, ""};
    plugin::ModManifest cycB{"mod.b", plugin::SemVer{1, 0, 0}, {{"mod.a", ">=1.0.0"}}, {}, 4 * 1024 * 1024, 100000, ""};
    const auto cycPlan = plugin::resolveModLoadPlan({cycA, cycB});
    RS_CHECK(!cycPlan.diagnostics.empty());
    bool sawCycle = false;
    for (const auto& d : cycPlan.diagnostics) {
        if (d.kind == plugin::ModPlanErrorKind::DependencyCycle) {
            sawCycle = true;
        }
    }
    RS_CHECK(sawCycle);

    // Version mismatch detection test.
    plugin::ModManifest badDep{
        "mod.bad",
        plugin::SemVer{1, 0, 0},
        {{"mod.core", plugin::VersionConstraint::parse(">=2.0.0").value()}},
        {}, 4 * 1024 * 1024, 100000, ""};
    const auto mismatchPlan = plugin::resolveModLoadPlan({core, badDep});
    RS_CHECK(!mismatchPlan.diagnostics.empty());
    bool sawMismatch = false;
    for (const auto& d : mismatchPlan.diagnostics) {
        if (d.kind == plugin::ModPlanErrorKind::VersionIncompatible) {
            sawMismatch = true;
        }
    }
    RS_CHECK(sawMismatch);

    std::cout << "[PASS] SemVer constraint solver, Kahn DAG planner & cycle diagnostics\n";
}

void testModHostSandboxQuotasCircuitBreakerAndMarshaling() {
    plugin::ModHostConfig cfg;
    cfg.faultThreshold = 2;
    cfg.mountReflectModuleByDefault = true;
    plugin::ModHost host(cfg);

    host.addCompilerExtensionPass(plugin::makeForbiddenImportPass({"Forbidden.Io"}));
    host.addCompilerExtensionPass(
        plugin::makeForbiddenSyntaxPass({syntax::SyntaxKind::ThrowStatement}));

    // 1) Good mod with "reflection" capability & endpoint/event handler.
    plugin::ModPackage modGood;
    modGood.manifest.id = "mod.good";
    modGood.manifest.version = {1, 0, 0};
    modGood.manifest.grantedCapabilities = {"reflection"};
    modGood.manifest.maxHeapBytes = 2 * 1024 * 1024;
    modGood.manifest.tickInstructionBudget = 50000;
    modGood.sources.push_back({
        "good.rscript",
        R"(module Mod.Good;
import RealScript.Reflect;

public class Badge {
    public int level = 5;
    public Badge() {}
}

public int scoreWithCap(int baseVal) {
    return Reflect.HasType("Mod.Good::Badge") ? (baseVal + 10) : -1;
}

public int onPing(string msg) {
    return msg == "ok" ? 77 : 0;
}

public int exhaustLoop() {
    while (true) {}
    return 0;
}
)",
    });

    // 2) Mod missing "reflection" capability (capability denial check).
    plugin::ModPackage modNoCap;
    modNoCap.manifest.id = "mod.nocap";
    modNoCap.manifest.version = {1, 0, 0};
    modNoCap.manifest.maxHeapBytes = 2 * 1024 * 1024;
    modNoCap.manifest.tickInstructionBudget = 50000;
    modNoCap.sources.push_back({
        "nocap.rscript",
        R"(module Mod.NoCap;
import RealScript.Reflect;

public bool tryReflect() {
    return Reflect.HasType("Mod.NoCap::Ghost");
}
)",
    });

    // 3) Mod with tight OOM heap budget + infinite fault trigger for circuit breaker.
    plugin::ModPackage modRogue;
    modRogue.manifest.id = "mod.rogue";
    modRogue.manifest.version = {1, 0, 0};
    modRogue.manifest.maxHeapBytes = 128 * 1024;
    modRogue.manifest.tickInstructionBudget = 5000;
    modRogue.sources.push_back({
        "rogue.rscript",
        R"(module Mod.Rogue;
public class Chunk { public int x = 1; public int y = 2; }
public int allocStorm() {
    var list = new Chunk[50000];
    return list.length;
}
public int spinTimeout() {
    int i = 0;
    while (true) { i = i + 1; }
    return i;
}
)",
    });

    // 4) Mod rejected by AST security extension pass (`Forbidden.Io` / `throw`).
    plugin::ModPackage modBlockedAst;
    modBlockedAst.manifest.id = "mod.blocked";
    modBlockedAst.manifest.version = {1, 0, 0};
    modBlockedAst.sources.push_back({
        "blocked.rscript",
        R"(module Mod.Blocked;
import Forbidden.Io;
public void bad() { throw "nope"; }
)",
    });

    // Transactional rejection: AST security pass rejects mod.blocked with compile diagnostics.
    const auto blockedReport = host.loadMods({modBlockedAst});
    RS_CHECK(!blockedReport.succeeded);
    RS_CHECK(blockedReport.compileDiagnostics.find("mod.blocked") !=
           blockedReport.compileDiagnostics.end());
    RS_CHECK(blockedReport.compileDiagnostics.at("mod.blocked").hasErrors());

    // Load the valid mod set so mod.good, mod.nocap, and mod.rogue activate cleanly.
    const auto report = host.loadMods({modGood, modNoCap, modRogue});
    RS_CHECK(report.succeeded);

    RS_CHECK(host.modStats("mod.good").has_value());
    RS_CHECK(host.modStats("mod.good")->state == plugin::ModState::Active);
    RS_CHECK(host.modStats("mod.rogue").has_value());
    RS_CHECK(host.modStats("mod.rogue")->state == plugin::ModState::Active);

    // Capability-gated Reflect check.
    const auto capOk = host.invokeModEndpoint("mod.good", "Mod.Good::scoreWithCap", {runtime::Value{std::int64_t{5}}});
    RS_CHECK(capOk.status == plugin::BusCallStatus::Succeeded);
    RS_CHECK(std::get<std::int64_t>(capOk.value) == 15);

    const auto capDenied = host.invokeModEndpoint("mod.nocap", "Mod.NoCap::tryReflect", {});
    RS_CHECK(capDenied.status != plugin::BusCallStatus::Succeeded);

    // Faults on rogue mod trip its circuit breaker after faultThreshold (2), isolating mod.good cleanly.
    const auto oomRes = host.invokeModEndpoint("mod.rogue", "Mod.Rogue::allocStorm", {});
    RS_CHECK(oomRes.status == plugin::BusCallStatus::RuntimeFault);

    const auto spinRes = host.invokeModEndpoint("mod.rogue", "Mod.Rogue::spinTimeout", {});
    RS_CHECK(spinRes.status == plugin::BusCallStatus::RuntimeFault);
    RS_CHECK(host.modStats("mod.rogue")->state == plugin::ModState::Tripped);

    const auto skippedRes = host.invokeModEndpoint("mod.rogue", "Mod.Rogue::spinTimeout", {});
    RS_CHECK(skippedRes.status == plugin::BusCallStatus::ModTripped);

    RS_CHECK(host.resetCircuitBreaker("mod.rogue"));
    RS_CHECK(host.modStats("mod.rogue")->state == plugin::ModState::Active);

    // Event bus marshaling across isolated heaps cleanly delivers strings and isolates faults.
    RS_CHECK(host.subscribeEvent("mod.good", "bus.ping", "Mod.Good::onPing"));
    const auto strVal = host.hostHeap().allocateString("ok");
    RS_CHECK(strVal.has_value());
    const auto pubReport = host.publishEvent("bus.ping", {runtime::Value{*strVal}}, &host.hostHeap());
    RS_CHECK(pubReport.succeeded);
    RS_CHECK(pubReport.deliveredCount == 1);
    RS_CHECK(pubReport.failedCount == 0);

    // Cross-heap deep copy of string succeeds; raw ObjectRef from foreign heap is isolated safely.
    auto goodHeap = host.modHeap("mod.good");
    auto rogueHeap = host.modHeap("mod.rogue");
    RS_CHECK(goodHeap && rogueHeap);
    const auto copied = plugin::marshalCrossHeapValue(
        runtime::Value{*strVal}, &host.hostHeap(), goodHeap.get());
    RS_CHECK(copied.succeeded);
    RS_CHECK(std::holds_alternative<runtime::ObjectRef>(copied.value));
    const auto view = goodHeap->stringView(std::get<runtime::ObjectRef>(copied.value));
    RS_CHECK(view.has_value() && *view == "ok");

    // Passing a heap Record ObjectRef or foreign Array ObjectRef is strictly rejected across heaps.
    const auto foreignRec = rogueHeap->allocateObject(
        1u,
        std::vector<runtime::Value>{runtime::Value{std::int64_t{7}}},
        std::vector<std::size_t>{});
    RS_CHECK(foreignRec.has_value());
    const auto badMarshal = plugin::marshalCrossHeapValue(
        runtime::Value{*foreignRec}, rogueHeap.get(), goodHeap.get());
    RS_CHECK(!badMarshal.succeeded);
    RS_CHECK(badMarshal.errorCode == plugin::MarshalErrorCode::CrossHeapReferenceRejected);

    const auto foreignArr = rogueHeap->allocateArray(2u, runtime::Value{std::int64_t{1}});
    RS_CHECK(foreignArr.has_value());
    const auto mismatchMarshal = plugin::marshalCrossHeapValue(
        runtime::Value{*foreignArr}, &host.hostHeap(), goodHeap.get());
    RS_CHECK(!mismatchMarshal.succeeded);
    RS_CHECK(mismatchMarshal.errorCode == plugin::MarshalErrorCode::InvalidSourceReference);

    // Good mod remains completely healthy throughout rogue faults.
    const auto goodAfter = host.invokeModEndpoint("mod.good", "Mod.Good::scoreWithCap", {runtime::Value{std::int64_t{30}}});
    RS_CHECK(goodAfter.status == plugin::BusCallStatus::Succeeded);
    RS_CHECK(std::get<std::int64_t>(goodAfter.value) == 40);

    std::cout << "[PASS] ModHost AST passes, capability gates, OOM/timeout isolation & marshaling\n";
}

void testModHostValuePoolDomainsAndUnload() {
    plugin::ModHost host;

    plugin::ModPackage modAlpha;
    modAlpha.manifest.id = "mod.alpha";
    modAlpha.manifest.version = {1, 0, 0};
    modAlpha.manifest.maxHeapBytes = 2u * 1024u * 1024u;
    modAlpha.manifest.tickInstructionBudget = 50000u;
    modAlpha.sources.push_back({
        "alpha.rscript",
        R"(module Mod.Alpha;
public string Greeting(string who) {
    return "alpha:" + who;
}
)",
    });

    plugin::ModPackage modBeta;
    modBeta.manifest.id = "mod.beta";
    modBeta.manifest.version = {1, 0, 0};
    modBeta.manifest.maxHeapBytes = 2u * 1024u * 1024u;
    modBeta.manifest.tickInstructionBudget = 50000u;
    modBeta.sources.push_back({
        "beta.rscript",
        R"(module Mod.Beta;
public string Greeting(string who) {
    return "beta:" + who;
}
)",
    });

    const auto loadRep = host.loadMods({modAlpha, modBeta});
    RS_CHECK(loadRep.succeeded);

    auto alphaHeap = host.modHeap("mod.alpha");
    auto betaHeap = host.modHeap("mod.beta");
    RS_CHECK(alphaHeap != nullptr && betaHeap != nullptr);
    RS_CHECK(alphaHeap->valuePoolDomain() != nullptr && betaHeap->valuePoolDomain() != nullptr);

    const std::uint8_t alphaId = alphaHeap->valuePoolDomain()->domainId();
    const std::uint8_t betaId = betaHeap->valuePoolDomain()->domainId();
    RS_CHECK(alphaId != 0u && betaId != 0u && alphaId != betaId);

    const auto argA = alphaHeap->allocateString("one");
    const auto argB = betaHeap->allocateString("two");
    RS_CHECK(argA.has_value() && argB.has_value());
    const auto resA = host.invokeModEndpoint("mod.alpha", "Mod.Alpha::Greeting", {runtime::Value{*argA}});
    const auto resB = host.invokeModEndpoint("mod.beta", "Mod.Beta::Greeting", {runtime::Value{*argB}});
    RS_CHECK(resA.status == plugin::BusCallStatus::Succeeded);
    RS_CHECK(resB.status == plugin::BusCallStatus::Succeeded);

    const std::weak_ptr<runtime::ValuePoolDomain> weakAlphaDomain = alphaHeap->valuePoolDomain();
    alphaHeap.reset();
    RS_CHECK(host.unloadMod("mod.alpha"));
    RS_CHECK(!host.modHeap("mod.alpha"));
    RS_CHECK(weakAlphaDomain.expired());

    RS_CHECK(host.unloadMod("mod.beta"));
    RS_CHECK(!host.modHeap("mod.beta"));

    std::cout << "[PASS] Pillar 1: ModHost distinct ValuePoolDomains & clean domain teardown on unload\n";
}

} // namespace

int main() {
    testReflectionMetadataAttributesAndDigest();
    testNonPublicEncapsulationEnforcement();
    testFastViewSafetyPolymorphismAndStaticConstructors();
    testSemVerAndKahnDagModPlanner();
    testModHostSandboxQuotasCircuitBreakerAndMarshaling();
    testModHostValuePoolDomainsAndUnload();
    std::cout << "All app_reflection_and_mod_host_tests passed!\n";
    return 0;
}
