#include "realscript/plugin/ModHost.h"
#include "realscript/reflection/Reflection.h"

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("Assertion failed: " + message);
    }
}

std::string formatDiagnostics(const diagnostics::DiagnosticBag& bag) {
    std::string out;
    for (const auto& item : bag.items()) {
        out += item.code + ": " + item.message + " (" + item.sourceName + ")\n";
    }
    return out;
}

std::shared_ptr<const runtime::ProgramImage> compileSources(
    const std::vector<compiler::SourceFile>& sources,
    optimization::Level optLevel = optimization::Level::None) {
    compiler::Compilation comp(sources);
    auto build = comp.build();
    require(
        !build.diagnostics.hasErrors(),
        "Compilation failed:\n" + formatDiagnostics(build.diagnostics));

    std::vector<mir::Module> mirModules = std::move(build.modules);
    if (optLevel != optimization::Level::None) {
        optimization::Options opts;
        opts.level = optLevel;
        auto optRes = optimization::Optimizer{}.optimize(
            std::move(mirModules), build.diagnostics, opts);
        require(
            !build.diagnostics.hasErrors(),
            "Optimization failed:\n" + formatDiagnostics(build.diagnostics));
        mirModules = std::move(optRes.modules);
    }

    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bcModules;
    bcModules.reserve(mirModules.size());
    for (const auto& m : mirModules) {
        bcModules.push_back(lowerer.lower(m));
    }

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcModules), linkErr);
    require(linked.has_value(), "Link failed: " + linkErr.message);
    return std::make_shared<const runtime::ProgramImage>(std::move(*linked));
}

std::string readHeapOrInlineString(
    const runtime::Value& v,
    const runtime::ManagedHeap* heap = nullptr) {
    if (const auto* s = std::get_if<std::string>(&v)) {
        return *s;
    }
    if (const auto* ref = std::get_if<runtime::ObjectRef>(&v)) {
        if (heap && ref->heapId == static_cast<std::uint32_t>(heap->heapId())) {
            if (const auto sv = heap->stringView(*ref)) {
                return std::string(*sv);
            }
        }
        if (const auto globalStr = runtime::ManagedHeap::tryCopyStringGlobal(*ref)) {
            return *globalStr;
        }
    }
    return {};
}

// ============================================================================
// 1. Deterministic ReflectionRegistry Metadata, Backing Fields & Attributes
// ============================================================================
void testReflectionMetadataAndAttributeQueries() {
    const std::string coreSrc = R"RS(
module Game.Core;

public interface IDamageable {
    int ApplyDamage(int rawAmount);
}

public interface IInspectable {
    string Describe();
}

[Export(category = "DTO", version = 2)]
public struct StatSnapshot {
    [Sync]
    public int hp;
    public double ratio;
    public string tag;

    public StatSnapshot(int h, double r, string t) {
        hp = h;
        ratio = r;
        tag = t;
    }
}

public enum FactionKind {
    Neutral = 0,
    Order = 1,
    Chaos = 2
}

public class ActorBase {
    public const int MaxLevel = 99;
    public static int SpawnCounter = 10;

    [Sync(priority = 1, label = "base_hp", scale = 1.5, reliable = true)]
    public int health = 100;

    public virtual int ComputePower(int bonus) {
        return health + bonus;
    }
}
)RS";

    const std::string combatSrc = R"RS(
module Game.Combat;
import Game.Core;

[Export("CombatUnit")]
[Replicate(interval = 0.25, active = true, count = 42, note = "line1\n\"quoted\"")]
public class Paladin : ActorBase, IDamageable, IInspectable {
    [Sync(priority = 2, reliable = false)]
    public int Shield { get; set; } = 50;

    public string title = "Templar";
    public ActorBase companion;

    public Paladin() {
        health = 150;
    }

    public Paladin(int customHp, string customTitle) {
        health = customHp;
        title = customTitle;
    }

    public override int ComputePower(int bonus) {
        return health + Shield + bonus * 2;
    }

    [Rpc(channel = "reliable", opcode = 108)]
    public int ApplyDamage(int rawAmount) {
        int mitigated = rawAmount - 5;
        if (mitigated < 0) mitigated = 0;
        health = health - mitigated;
        return health;
    }

    public string Describe() {
        return title;
    }

    [Rpc(channel = "unreliable", opcode = 204)]
    public static int CombineShield(int a, int b) {
        return a + b;
    }
}

public class UnrelatedCrate {
    public int weight = 25;
}

[Rpc(channel = "system", opcode = 900)]
public int GlobalCombatPing(int seq) {
    return seq + 1;
}
)RS";

    auto progForward = compileSources({
        {"Game.Core.rscript", coreSrc},
        {"Game.Combat.rscript", combatSrc},
    });
    auto progReverse = compileSources({
        {"Game.Combat.rscript", combatSrc},
        {"Game.Core.rscript", coreSrc},
    });

    reflection::ReflectionRegistry reg1(progForward);
    reflection::ReflectionRegistry reg2(progReverse);

    require(
        reg1.metadataDigest() != 0 && reg1.metadataDigest() == reg2.metadataDigest(),
        "ReflectionRegistry digest must be 100% deterministic regardless of source order");
    require(
        reg1.types().size() == reg2.types().size(),
        "ReflectionRegistry type count mismatch");
    for (std::size_t i = 0; i < reg1.types().size(); ++i) {
        require(
            reg1.types()[i].canonicalName == reg2.types()[i].canonicalName,
            "ReflectionRegistry type ordering is not deterministic");
    }

    // Lookup by canonical name, short name, and SymbolId
    const auto* paladinByCanonical = reg1.findType("Game.Combat::Paladin");
    const auto* paladinByShort = reg1.findType("Paladin");
    require(paladinByCanonical != nullptr && paladinByCanonical == paladinByShort,
        "Paladin lookup by canonical and short name must resolve identically");
    const auto* paladinById = reg1.findType(paladinByCanonical->typeId);
    require(paladinById == paladinByCanonical, "Paladin lookup by SymbolId failed");

    require(paladinByCanonical->moduleName == "Game.Combat", "wrong moduleName");
    require(paladinByCanonical->name == "Paladin", "wrong short name");
    require(paladinByCanonical->kind == semantic::TypeKind::Class, "wrong TypeKind");
    const auto* actorBase = reg1.findType("ActorBase");
    require(actorBase != nullptr, "ActorBase not found");
    require(paladinByCanonical->baseTypeId == actorBase->typeId, "wrong baseTypeId");

    require(
        paladinByCanonical->implementedInterfaces.size() == 2 &&
            paladinByCanonical->implementedInterfaces[0] == "Game.Core::IDamageable" &&
            paladinByCanonical->implementedInterfaces[1] == "Game.Core::IInspectable",
        "implementedInterfaces must be deterministically sorted and complete");

    // Verify AttributeReflection typed & unquoted readers + type safety
    const auto* repAttr = paladinByCanonical->findAttribute("Replicate");
    require(repAttr != nullptr, "Replicate attribute missing on Paladin");
    require(repAttr->getDouble("interval").has_value() &&
            std::abs(*repAttr->getDouble("interval") - 0.25) < 1e-9,
        "Replicate.interval getDouble failed");
    require(repAttr->getBool("active") == std::optional<bool>{true},
        "Replicate.active getBool failed");
    require(repAttr->getInt("count") == std::optional<std::int64_t>{42},
        "Replicate.count getInt failed");
    require(repAttr->getString("note") == std::optional<std::string>{"line1\n\"quoted\""},
        "Replicate.note getString unquoting/unescaping failed");

    // Type-safety checks on AttributeReflection
    require(!repAttr->getInt("note").has_value(), "getInt must reject string literal");
    require(!repAttr->getBool("count").has_value(), "getBool must reject integer literal");
    require(!repAttr->getString("count").has_value(), "getString must reject unquoted integer");
    require(!repAttr->getString("active").has_value(), "getString must reject boolean literal");
    require(!repAttr->getInt("interval").has_value(), "getInt must reject floating-point literal");

    const auto* expAttr = paladinByCanonical->findAttribute("Export");
    require(expAttr != nullptr && expAttr->getString() == std::optional<std::string>{"CombatUnit"},
        "Positional arg0 getString failed on [Export(\"CombatUnit\")]");

    // Verify property backing field reflection
    const auto* shieldProp = paladinByCanonical->findProperty("Shield");
    require(shieldProp != nullptr && shieldProp->hasBackingField,
        "Auto-property Shield must expose backing field metadata");
    const auto* shieldField = paladinByCanonical->findField("Shield");
    require(shieldField != nullptr && shieldField->isBackingField &&
            shieldField->backedPropertyName == "Shield" &&
            shieldField->offsetIndex == shieldProp->backingFieldIndex,
        "FieldReflection backing field mapping mismatch");
    require(shieldField->hasAttribute("Sync"),
        "Auto-property backing field should inherit [Sync] attribute for unified field queries");

    // Verify global attribute queries ([Export], [Sync], [Rpc])
    const auto exportedTypes = reg1.findTypesWithAttribute("Export");
    require(exportedTypes.size() == 2 &&
            exportedTypes[0].type->canonicalName == "Game.Combat::Paladin" &&
            exportedTypes[1].type->canonicalName == "Game.Core::StatSnapshot",
        "findTypesWithAttribute(\"Export\") mismatch");

    const auto syncedFields = reg1.findFieldsWithAttribute("Sync");
    require(syncedFields.size() == 3,
        "Expected 3 unique [Sync] fields (Paladin.$Shield, ActorBase.health, StatSnapshot.hp)");

    const auto rpcMethods = reg1.findMethodsWithAttribute("Rpc");
    require(rpcMethods.size() == 3,
        "Expected 3 [Rpc] methods/functions (ApplyDamage, CombineShield, GlobalCombatPing)");
    require(rpcMethods[0].attribute->getInt("opcode") == std::optional<std::int64_t>{108},
        "ApplyDamage Rpc opcode mismatch");
}

// ============================================================================
// 2. Dynamic Instantiation, ObjectFastView Invariant & Polymorphic Dispatch
// ============================================================================
void testDynamicOperationsAndFastViewSafety() {
    const std::string src = R"RS(
module Game.Dyn;

public interface IAction {
    int Execute(int factor);
}

public struct PairDto {
    public int left;
    public string label;

    public PairDto(int l, string s) {
        left = l;
        label = s;
    }
}

public class BaseAgent {
    public const int VersionTag = 7;
    public static int GlobalCount = 3;
    public int energy = 20;
    public string codeName = "init";

    public BaseAgent() {
        energy = 25;
    }

    public virtual int Step(int delta) {
        return energy + delta;
    }
}

public class RogueAgent : BaseAgent, IAction {
    public int stealth = 10;
    public BaseAgent partner;

    public RogueAgent() {
        energy = 40;
        stealth = 15;
    }

    public RogueAgent(int e, int s, string name) {
        energy = e;
        stealth = s;
        codeName = name;
    }

    public override int Step(int delta) {
        return energy + stealth * 2 + delta;
    }

    public int Execute(int factor) {
        return (energy + stealth) * factor;
    }
}

public class DecoyBox {
    public int dummy = 1;
}

public int ReadEnergyFastPath(BaseAgent agent) {
    // Exercises VM LoadFieldFast / ObjectFastView assumptions on BaseAgent.energy & codeName
    return agent.energy + agent.codeName.Length();
}

public static class MathUtil {
    public static int Triple(int x) {
        return x * 3;
    }
}
)RS";

    auto program = compileSources({{"Game.Dyn.rscript", src}}, optimization::Level::Aggressive);
    runtime::EngineRuntime engine(program);
    reflection::ReflectionRegistry refl(engine);

    runtime::RuntimeError err;
    // Instantiate without running constructor vs with parameterized constructor
    auto uninitRef = refl.instantiateObject(engine, "RogueAgent", {}, err, /*runConstructor=*/false);
    require(uninitRef.has_value(), "uninitialized instantiateObject failed: " + err.message);
    auto uninitRoot = engine.heap()->retain(*uninitRef);
    auto uninitEnergy = refl.getField(engine, *uninitRef, "energy", err);
    require(uninitEnergy.has_value() && std::get<std::int64_t>(*uninitEnergy) == 20,
        "field initializer value should be 20 before constructor runs");

    auto rogueRef = refl.instantiateObject(
        engine,
        "Game.Dyn::RogueAgent",
        {runtime::Value{std::int64_t{100}}, runtime::Value{std::int64_t{30}}, runtime::Value{"Shadow"}},
        err,
        /*runConstructor=*/true);
    require(rogueRef.has_value(), "parameterized instantiateObject failed: " + err.message);
    auto rogueRoot = engine.heap()->retain(*rogueRef);

    auto eVal = refl.getField(engine, *rogueRef, "energy", err);
    auto sVal = refl.getField(engine, *rogueRef, "stealth", err);
    auto nVal = refl.getField(engine, *rogueRef, "codeName", err);
    require(eVal && std::get<std::int64_t>(*eVal) == 100, "ctor energy mismatch");
    require(sVal && std::get<std::int64_t>(*sVal) == 30, "ctor stealth mismatch");
    require(nVal && readHeapOrInlineString(*nVal, engine.heap().get()) == "Shadow",
        "ctor codeName mismatch");

    // Strict ObjectFastView type-safety rejections on setField:
    // 1) Reject writing string into int field
    err = {};
    require(
        !refl.setField(engine, *rogueRef, "energy", runtime::Value{"corrupt"}, err) &&
            err.code == runtime::ErrorCode::TypeMismatch,
        "setField must reject writing string into int field");

    // 2) Reject writing int into string field
    err = {};
    require(
        !refl.setField(engine, *rogueRef, "codeName", runtime::Value{std::int64_t{999}}, err) &&
            err.code == runtime::ErrorCode::TypeMismatch,
        "setField must reject writing int into string field");

    // 3) Reject writing unrelated class DecoyBox into BaseAgent-typed reference field `partner`
    auto decoyRef = refl.instantiateObject(engine, "DecoyBox", {}, err);
    require(decoyRef.has_value(), "DecoyBox instantiate failed");
    auto decoyRoot = engine.heap()->retain(*decoyRef);
    err = {};
    require(
        !refl.setField(engine, *rogueRef, "partner", runtime::Value{*decoyRef}, err) &&
            err.code == runtime::ErrorCode::TypeMismatch,
        "setField must reject unrelated class reference assignment");

    // 4) Reject writing ObjectRef from a foreign ManagedHeap!
    runtime::ManagedHeap foreignHeap;
    auto foreignRogue = refl.instantiateUninitializedObject(foreignHeap, "RogueAgent", err);
    require(foreignRogue.has_value(), "foreign heap instantiate failed");
    auto foreignRoot = foreignHeap.retain(*foreignRogue);
    err = {};
    require(
        !refl.setField(engine, *rogueRef, "partner", runtime::Value{*foreignRogue}, err) &&
            err.code == runtime::ErrorCode::InvalidObjectReference,
        "setField must reject foreign-heap ObjectRef");

    // 5) Allow derived class RogueAgent to be assigned into BaseAgent-typed `partner`!
    auto allyRef = refl.instantiateObject(engine, "RogueAgent", {}, err);
    require(allyRef.has_value(), "ally RogueAgent instantiate failed");
    auto allyRoot = engine.heap()->retain(*allyRef);
    err = {};
    require(
        refl.setField(engine, *rogueRef, "partner", runtime::Value{*allyRef}, err),
        "setField must allow derived class assignment to base class field: " + err.message);

    // 6) Verify const field is read-only, while valid setField preserves ObjectFastView fast path
    err = {};
    require(
        !refl.setField(engine, *rogueRef, "VersionTag", runtime::Value{std::int64_t{8}}, err),
        "const field write must be rejected");
    require(
        refl.setField(engine, *rogueRef, "energy", runtime::Value{std::int64_t{120}}, err),
        "valid int setField failed");
    require(
        refl.setField(engine, *rogueRef, "codeName", runtime::Value{"Ghost"}, err),
        "valid string setField failed");

    const auto fastPathRes = engine.invoke("Game.Dyn::ReadEnergyFastPath", {runtime::Value{*rogueRef}});
    require(
        fastPathRes.succeeded && std::get<std::int64_t>(fastPathRes.value) == 125,
        "VM LoadFieldFast / ObjectFastView execution failed after reflected setField");

    // Static fields & StructValue fields
    err = {};
    auto initGlobal = refl.getStaticField(*engine.heap(), "BaseAgent", "GlobalCount", err, &engine);
    require(initGlobal && std::get<std::int64_t>(*initGlobal) == 3, "static field initial read failed");
    require(
        refl.setStaticField(*engine.heap(), "BaseAgent", "GlobalCount", runtime::Value{std::int64_t{42}}, err, &engine),
        "static field write failed");
    auto updatedGlobal = refl.getStaticField(*engine.heap(), "BaseAgent", "GlobalCount", err, &engine);
    require(updatedGlobal && std::get<std::int64_t>(*updatedGlobal) == 42, "static field updated read failed");

    const auto* pairType = refl.findType("PairDto");
    require(pairType != nullptr, "PairDto not found");
    runtime::StructValue dto = runtime::makeStructValueFromFields(
        pairType->typeId,
        std::vector<runtime::Value>{runtime::Value{std::int64_t{11}}, runtime::Value{"alpha"}});
    auto dtoLeft = refl.getField(dto, "left", err);
    require(dtoLeft && std::get<std::int64_t>(*dtoLeft) == 11, "StructValue getField failed");
    require(!refl.setField(dto, "left", runtime::Value{"bad"}, err),
        "StructValue setField must reject wrong type");
    require(refl.setField(dto, "left", runtime::Value{std::int64_t{88}}, err),
        "StructValue setField failed: " + err.message);
    require(std::get<std::int64_t>(*refl.getField(dto, "left", err)) == 88,
        "StructValue updated field mismatch");

    // Dynamic polymorphic virtual & interface method dispatch!
    auto baseInstance = refl.instantiateObject(engine, "BaseAgent", {}, err);
    require(baseInstance.has_value(), "BaseAgent instantiate failed");
    auto baseRoot = engine.heap()->retain(*baseInstance);

    const auto* baseType = refl.findType("BaseAgent");
    const auto* baseStepMethod = baseType->findMethod("Step", 1u);
    require(baseStepMethod != nullptr && baseStepMethod->virtualSlot.has_value(),
        "BaseAgent.Step must have a virtualSlot");

    // Calling BaseAgent::Step descriptor on BaseAgent -> 25 + 5 = 30
    auto stepOnBase = refl.invokeMethod(
        engine, *baseStepMethod, *baseInstance, {runtime::Value{std::int64_t{5}}});
    require(stepOnBase.succeeded && std::get<std::int64_t>(stepOnBase.value) == 30,
        "BaseAgent virtual call on base instance failed");

    // Calling BaseAgent::Step descriptor on RogueAgent (energy=120, stealth=30) -> 120 + 60 + 5 = 185!
    auto stepOnDerived = refl.invokeMethod(
        engine, *baseStepMethod, *rogueRef, {runtime::Value{std::int64_t{5}}});
    require(stepOnDerived.succeeded && std::get<std::int64_t>(stepOnDerived.value) == 185,
        "BaseAgent virtual call on RogueAgent must dynamically dispatch to RogueAgent.Step");

    // Calling IAction::Execute interface descriptor on RogueAgent -> (120 + 30) * 3 = 450!
    const auto* actionIface = refl.findType("IAction");
    require(actionIface != nullptr && actionIface->isInterface, "IAction interface not found");
    const auto* ifaceExecMethod = actionIface->findMethod("Execute", 1u);
    require(ifaceExecMethod != nullptr && ifaceExecMethod->interfaceSlot.has_value(),
        "IAction.Execute must have an interfaceSlot");
    auto ifaceRes = refl.invokeMethod(
        engine, *ifaceExecMethod, *rogueRef, {runtime::Value{std::int64_t{3}}});
    require(ifaceRes.succeeded && std::get<std::int64_t>(ifaceRes.value) == 450,
        "Interface polymorphic invokeMethod failed");

    // Static method invocation
    auto staticRes = refl.invokeMethod(
        engine, "MathUtil", "Triple", {runtime::Value{std::int64_t{14}}});
    require(staticRes.succeeded && std::get<std::int64_t>(staticRes.value) == 42,
        "Static method invokeMethod failed");
}

// ============================================================================
// 3. Script-Side `RealScript.Reflect` Stub, Determinism & Capability Gate
// ============================================================================
void testScriptReflectCapabilityAndDeterminism() {
    const std::string scriptSrc = R"RS(
module ScriptReflectApp;
import RealScript.Reflect;

[PluginTag(author = "Ada", revision = 3)]
public class BaseItem {
    [ConfigKey("base_cost")]
    public int cost = 10;
    public string name = "Potion";
}

public class RareItem : BaseItem {
    public int rarity = 5;

    public RareItem() {
        cost = 50;
        name = "Elixir";
    }
}

public int VerifyScriptReflection() {
    if (!Reflect.HasType("ScriptReflectApp::RareItem")) return -1;
    if (Reflect.HasType("ScriptReflectApp::NonExistent")) return -2;

    if (!Reflect.HasAttribute("ScriptReflectApp::BaseItem", "PluginTag")) return -3;
    if (!Reflect.HasAttribute("BaseItem.cost", "ConfigKey")) return -4;

    string author = Reflect.GetAttributeString("ScriptReflectApp::BaseItem", "PluginTag", "author");
    if (author != "Ada") return -5;

    string cfgKey = Reflect.GetAttributeString("BaseItem.cost", "ConfigKey", "arg0");
    if (cfgKey != "base_cost") return -6;

    int fcount = Reflect.GetFieldCount("RareItem");
    if (fcount != 3) return -7;
    if (Reflect.GetFieldName("RareItem", 0) != "cost") return -8;
    if (Reflect.GetFieldName("RareItem", 1) != "name") return -9;
    if (Reflect.GetFieldName("RareItem", 2) != "rarity") return -10;

    // Runtime polymorphic TypeNameOf even through BaseItem static reference!
    BaseItem poly = new RareItem();
    string runtimeType = Reflect.TypeNameOf(poly);
    if (runtimeType != "ScriptReflectApp::RareItem") return -11;

    int initialCost = Reflect.GetIntField(poly, "cost");
    if (initialCost != 50) return -12;

    Reflect.SetIntField(poly, "cost", 135);
    if (poly.cost != 135) return -13;

    string initialName = Reflect.GetStringField(poly, "name");
    if (initialName != "Elixir") return -14;

    Reflect.SetStringField(poly, "name", "PhoenixTear");
    if (poly.name != "PhoenixTear") return -15;

    return poly.cost + poly.name.Length();
}
)RS";

    auto program = compileSources({
        reflection::exportReflectDeclarationStub(),
        {"ScriptReflectApp.rscript", scriptSrc},
    });

    auto makeConfiguredEngine = [&program]() {
        auto eng = std::make_unique<runtime::EngineRuntime>(program);
        auto refl = std::make_shared<reflection::ReflectionRegistry>(*eng);
        auto bindings = std::make_shared<runtime::BindingRegistry>();
        reflection::registerReflectBindings(*bindings, refl, "reflection");
        eng->setBindings(bindings);
        return eng;
    };

    auto deniedEngine = makeConfiguredEngine();

    // 1) Denied when enforceCapabilities = true without "reflection"
    runtime::ExecutionOptions deniedOpts;
    deniedOpts.enforceCapabilities = true;
    deniedOpts.grantedCapabilities = {"ui"};
    const auto deniedRes = deniedEngine->invoke("ScriptReflectApp::VerifyScriptReflection", {}, deniedOpts);
    require(
        !deniedRes.succeeded && deniedRes.error.code == runtime::ErrorCode::CapabilityDenied,
        "Expected CapabilityDenied when 'reflection' capability is not granted");

    // 2) Allowed & 100% deterministic in Strict mode across independent replays when "reflection" IS granted
    runtime::ExecutionOptions strictAllowed1;
    strictAllowed1.enforceCapabilities = true;
    strictAllowed1.grantedCapabilities = {"reflection"};
    strictAllowed1.determinism.mode = runtime::DeterminismMode::Strict;

    runtime::ExecutionOptions strictAllowed2 = strictAllowed1;

    auto engine1 = makeConfiguredEngine();
    auto engine2 = makeConfiguredEngine();

    const auto res1 = engine1->invoke("ScriptReflectApp::VerifyScriptReflection", {}, strictAllowed1);
    require(
        res1.succeeded,
        "Script reflection failed with capability granted: " + res1.error.message +
            " (code=" + std::string(runtime::errorCodeName(res1.error.code)) + ")");
    require(
        std::get<std::int64_t>(res1.value) == 135 + 11,
        "Unexpected script reflection result: " + std::to_string(std::get<std::int64_t>(res1.value)));

    const auto res2 = engine2->invoke("ScriptReflectApp::VerifyScriptReflection", {}, strictAllowed2);
    require(
        res2.succeeded && std::get<std::int64_t>(res2.value) == 135 + 11 &&
            res1.determinismDigest == res2.determinismDigest,
        "RealScript.Reflect calls must be 100% deterministic under DeterminismMode::Strict");
}

// ============================================================================
// 4. SemVer, Version Constraints, JSON Manifest & Kahn Topological Solver
// ============================================================================
void testSemVerAndTopologicalDependencySolver() {
    // SemVer parsing & ordering
    const auto v123 = plugin::SemVer::parse("1.2.3");
    require(v123.has_value() && v123->major == 1 && v123->minor == 2 && v123->patch == 3,
        "SemVer 1.2.3 parse failed");
    require(!plugin::SemVer::parse("1.2").has_value(), "SemVer must reject 2-part version");
    require(!plugin::SemVer::parse("01.2.3").has_value(), "SemVer must reject leading zero");
    require(!plugin::SemVer::parse("1.2.3.4").has_value(), "SemVer must reject 4-part version");
    require(*plugin::SemVer::parse("1.2.3") < *plugin::SemVer::parse("1.10.0"),
        "SemVer numeric comparison failed");

    // Caret, tilde, range, exact, wildcard constraints
    const auto caret1 = plugin::VersionConstraint::parse("^1.2.0");
    require(caret1.has_value(), "Failed to parse ^1.2.0");
    require(caret1->satisfies({1, 2, 0}) && caret1->satisfies({1, 9, 5}) &&
            !caret1->satisfies({1, 1, 9}) && !caret1->satisfies({2, 0, 0}),
        "^1.2.0 evaluation incorrect");

    const auto caret0 = plugin::VersionConstraint::parse("^0.2.3");
    require(caret0.has_value() && caret0->satisfies({0, 2, 9}) && !caret0->satisfies({0, 3, 0}),
        "^0.2.3 zero-major evaluation incorrect");

    const auto tilde = plugin::VersionConstraint::parse("~1.2.3");
    require(tilde.has_value() && tilde->satisfies({1, 2, 9}) &&
            !tilde->satisfies({1, 3, 0}) && !tilde->satisfies({1, 2, 2}),
        "~1.2.3 evaluation incorrect");

    const auto range = plugin::VersionConstraint::parse(">=1.0.0 <2.0.0");
    require(range.has_value() && range->satisfies({1, 0, 0}) &&
            range->satisfies({1, 99, 99}) && !range->satisfies({2, 0, 0}),
        ">=1.0.0 <2.0.0 evaluation incorrect");

    const auto exact = plugin::VersionConstraint::parse("=1.0.0");
    require(exact.has_value() && exact->satisfies({1, 0, 0}) && !exact->satisfies({1, 0, 1}),
        "=1.0.0 evaluation incorrect");

    const auto star = plugin::VersionConstraint::parse("*");
    require(star.has_value() && star->satisfies({0, 0, 1}) && star->satisfies({99, 0, 0}),
        "* evaluation incorrect");

    // JSON Manifest parsing
    const std::string manifestJson = R"JSON({
        "id": "combat_plus",
        "version": "1.4.2",
        "dependencies": {
            "core_lib": "^1.2.0",
            "math_ext": ">=1.0.0 <2.0.0"
        },
        "grantedCapabilities": ["reflection", "combat.events"],
        "maxHeapBytes": 262144,
        "tickInstructionBudget": 50000,
        "entryModule": "Mods.CombatPlus"
    })JSON";
    std::string jsonErr;
    plugin::ModManifest parsedManifest;
    require(
        plugin::parseModManifestJson(manifestJson, parsedManifest, jsonErr),
        "parseModManifestJson failed: " + jsonErr);
    require(parsedManifest.id == "combat_plus" &&
            parsedManifest.version == plugin::SemVer{1, 4, 2} &&
            parsedManifest.dependencies.size() == 2 &&
            parsedManifest.grantedCapabilities.count("reflection") == 1 &&
            parsedManifest.maxHeapBytes == 262144 &&
            parsedManifest.tickInstructionBudget == 50000 &&
            parsedManifest.entryModule == "Mods.CombatPlus",
        "Parsed ModManifest fields mismatch");

    // Deterministic lexicographical Kahn topological order
    const auto makeMod = [](std::string id, plugin::SemVer v,
                            std::vector<std::pair<std::string, std::string>> deps) {
        plugin::ModManifest m;
        m.id = std::move(id);
        m.version = v;
        for (const auto& [dId, expr] : deps) {
            m.dependencies.push_back({dId, *plugin::VersionConstraint::parse(expr)});
        }
        return m;
    };

    const std::vector<plugin::ModManifest> dag1 = {
        makeMod("ui_hud", {1, 0, 0}, {{"combat", "^1.0.0"}, {"economy", "~2.1.0"}}),
        makeMod("economy", {2, 1, 4}, {{"core", ">=1.0.0 <2.0.0"}}),
        makeMod("combat", {1, 3, 0}, {{"core", "^1.2.0"}}),
        makeMod("analytics", {0, 5, 0}, {}),
        makeMod("core", {1, 2, 5}, {}),
    };
    std::vector<plugin::ModManifest> dag2 = dag1;
    std::reverse(dag2.begin(), dag2.end());

    const auto plan1 = plugin::resolveModLoadPlan(dag1);
    const auto plan2 = plugin::resolveModLoadPlan(dag2);
    require(plan1.succeeded && plan2.succeeded, "Valid Mod DAG must succeed");
    const std::vector<std::string> expectedOrder = {
        "analytics", "core", "combat", "economy", "ui_hud"};
    require(
        plan1.loadOrder == expectedOrder && plan2.loadOrder == expectedOrder,
        "Lexicographical Kahn topological loadOrder mismatch");

    // Error diagnostics: DuplicateModId, MissingDependency, VersionIncompatible, DependencyCycle
    const auto dupPlan = plugin::resolveModLoadPlan({
        makeMod("dup_mod", {1, 0, 0}, {}),
        makeMod("dup_mod", {1, 1, 0}, {}),
    });
    require(!dupPlan.succeeded && !dupPlan.diagnostics.empty() &&
            dupPlan.diagnostics[0].kind == plugin::ModPlanErrorKind::DuplicateModId,
        "Expected DuplicateModId diagnostic");

    const auto missingPlan = plugin::resolveModLoadPlan({
        makeMod("addon", {1, 0, 0}, {{"missing_base", "^1.0.0"}}),
    });
    require(!missingPlan.succeeded && !missingPlan.diagnostics.empty() &&
            missingPlan.diagnostics[0].kind == plugin::ModPlanErrorKind::MissingDependency,
        "Expected MissingDependency diagnostic");

    const auto verPlan = plugin::resolveModLoadPlan({
        makeMod("core", {2, 0, 0}, {}),
        makeMod("addon", {1, 0, 0}, {{"core", "^1.2.0"}}),
    });
    require(!verPlan.succeeded && !verPlan.diagnostics.empty() &&
            verPlan.diagnostics[0].kind == plugin::ModPlanErrorKind::VersionIncompatible,
        "Expected VersionIncompatible diagnostic");

    // Cycle A -> B -> C -> A
    const auto cyclePlan = plugin::resolveModLoadPlan({
        makeMod("A", {1, 0, 0}, {{"B", "*"}}),
        makeMod("B", {1, 0, 0}, {{"C", "*"}}),
        makeMod("C", {1, 0, 0}, {{"A", "*"}}),
        makeMod("RootOk", {1, 0, 0}, {}),
    });
    require(!cyclePlan.succeeded && !cyclePlan.diagnostics.empty() &&
            cyclePlan.diagnostics[0].kind == plugin::ModPlanErrorKind::DependencyCycle,
        "Expected DependencyCycle diagnostic");
    const std::vector<std::string> expectedCycle = {"A", "B", "C", "A"};
    require(
        cyclePlan.diagnostics[0].cyclePath == expectedCycle &&
            cyclePlan.diagnostics[0].message.find("A -> B -> C -> A") != std::string::npos,
        "Cycle path must report exact A -> B -> C -> A chain, got: " +
            cyclePlan.diagnostics[0].message);
}

// ============================================================================
// 5. Sandboxed ModHost: Compiler Hooks, Heap OOM & Infinite Loop Circuit Breaker
// ============================================================================
void testModHostSandboxIsolationAndCompilerPasses() {
    // 5a. Verify AST security passes (forbidden import & forbidden syntax node)
    {
        plugin::ModHost guardedHost;
        guardedHost.addCompilerExtensionPass(
            plugin::makeForbiddenImportPass({"Engine.UnsafeFs"}));
        guardedHost.addCompilerExtensionPass(
            plugin::makeForbiddenSyntaxPass({syntax::SyntaxKind::ThrowStatement}));

        plugin::ModPackage badImportPkg;
        badImportPkg.manifest.id = "rogue_import";
        badImportPkg.manifest.version = {1, 0, 0};
        badImportPkg.sources = {{
            "rogue_import.rscript",
            "module Mod.RogueImport;\nimport Engine.UnsafeFs;\npublic int Run() { return 1; }\n"}};

        const auto rep1 = guardedHost.loadMods({badImportPkg});
        require(!rep1.succeeded && rep1.compileDiagnostics.count("rogue_import") == 1,
            "Forbidden import must be blocked by CompilerExtensionPass");

        plugin::ModPackage badSyntaxPkg;
        badSyntaxPkg.manifest.id = "rogue_throw";
        badSyntaxPkg.manifest.version = {1, 0, 0};
        badSyntaxPkg.sources = {{
            "rogue_throw.rscript",
            R"RS(
module Mod.RogueThrow;
public class CustomErr { public CustomErr() {} }
public int Run() { throw new CustomErr(); }
)RS"}};

        const auto rep2 = guardedHost.loadMods({badSyntaxPkg});
        require(!rep2.succeeded && rep2.compileDiagnostics.count("rogue_throw") == 1,
            "Forbidden ThrowStatement syntax must be blocked by CompilerExtensionPass");
    }

    // 5b. Multi-Mod Isolation: Healthy Mods vs Infinite-Loop Mod vs Memory-Bomb OOM Mod
    plugin::ModHost host;

    // Mount read-only host ambient module `Host.Env` with capability-protected function
    const compiler::SourceFile hostEnvStub{
        "Host.Env.d.rscript",
        R"RS(
module Host.Env;
public int ReadSecretKey();
public int AddHostMetric(int delta);
)RS"};
    std::int64_t hostMetricAccumulator = 0;
    auto hostEnvBindings = std::make_shared<runtime::BindingRegistry>();
    (void)hostEnvBindings->bind(
        "Host.Env::ReadSecretKey",
        [](const bytecode::FunctionReference&, runtime::ValueSpan, runtime::RuntimeError&)
            -> std::optional<runtime::Value> {
            return runtime::Value{std::int64_t{7777}};
        },
        runtime::BindingDeterminism::Deterministic,
        "host.secret");
    (void)hostEnvBindings->bind(
        "Host.Env::AddHostMetric",
        [&hostMetricAccumulator](
            const bytecode::FunctionReference&,
            runtime::ValueSpan args,
            runtime::RuntimeError&) -> std::optional<runtime::Value> {
            if (!args.empty()) {
                std::int64_t d = 0;
                if (args[0].tryGetInt64(d)) hostMetricAccumulator += d;
            }
            return runtime::Value{hostMetricAccumulator};
        },
        runtime::BindingDeterminism::Deterministic,
        "host.telemetry");

    host.registerHostModule("Host.Env", hostEnvStub, hostEnvBindings);

    // 1. Healthy Mod A (granted host.secret + host.telemetry + reflection)
    plugin::ModPackage modGoodA;
    modGoodA.manifest.id = "mod_a_healthy";
    modGoodA.manifest.version = {1, 0, 0};
    modGoodA.manifest.grantedCapabilities = {"host.secret", "host.telemetry", "reflection"};
    modGoodA.manifest.maxHeapBytes = 512 * 1024;
    modGoodA.manifest.tickInstructionBudget = 20'000;
    modGoodA.manifest.entryModule = "ModA";
    modGoodA.sources = {{
        "ModA.rscript",
        R"RS(
module ModA;
import Host.Env;
import RealScript.Reflect;

[Export]
public class ModAState {
    public static int ticks = 0;
    public int tag = 9;
}

[EventHandler("OnWorldTick")]
public void HandleTick(int step) {
    ModAState.ticks = ModAState.ticks + step;
    AddHostMetric(step);
}

[ModEndpoint("QueryStatus")]
public int QueryStatus() {
    ModAState st = new ModAState();
    int secret = ReadSecretKey();
    int fieldVal = Reflect.GetIntField(st, "tag");
    return ModAState.ticks + secret + fieldVal;
}
)RS"}};

    // 2. Healthy Mod B (NOT granted host.secret -> unauthorized call gets CapabilityDenied, normal endpoints work)
    plugin::ModPackage modGoodB;
    modGoodB.manifest.id = "mod_b_restricted";
    modGoodB.manifest.version = {1, 0, 0};
    modGoodB.manifest.dependencies = {{"mod_a_healthy", "^1.0.0"}};
    modGoodB.manifest.grantedCapabilities = {"host.telemetry"};
    modGoodB.manifest.maxHeapBytes = 512 * 1024;
    modGoodB.manifest.tickInstructionBudget = 20'000;
    modGoodB.manifest.entryModule = "ModB";
    modGoodB.sources = {{
        "ModB.rscript",
        R"RS(
module ModB;
import Host.Env;

public class Counter {
    public static int sum = 0;
}

[EventHandler("OnWorldTick")]
public void HandleTick(int step) {
    Counter.sum = Counter.sum + step * 10;
    AddHostMetric(step * 2);
}

public int TryReadSecret() {
    return ReadSecretKey();
}

public int GetSum() {
    return Counter.sum;
}
)RS"}};

    // 3. Malicious Infinite-Loop Mod (tight budget = 2000 instructions)
    plugin::ModPackage modInfiniteLoop;
    modInfiniteLoop.manifest.id = "mod_c_infinite_loop";
    modInfiniteLoop.manifest.version = {1, 0, 0};
    modInfiniteLoop.manifest.maxHeapBytes = 256 * 1024;
    modInfiniteLoop.manifest.tickInstructionBudget = 2'000;
    modInfiniteLoop.manifest.entryModule = "ModLoop";
    modInfiniteLoop.sources = {{
        "ModLoop.rscript",
        R"RS(
module ModLoop;

[EventHandler("OnWorldTick")]
public void SpinForever(int step) {
    int x = step;
    while (true) {
        x = x + 1;
    }
}
)RS"}};

    // 4. Malicious Memory-Hog Mod (tiny maxHeapBytes = 8192 bytes)
    plugin::ModPackage modOom;
    modOom.manifest.id = "mod_d_memory_bomb";
    modOom.manifest.version = {1, 0, 0};
    modOom.manifest.maxHeapBytes = 8 * 1024;
    modOom.manifest.tickInstructionBudget = 50'000;
    modOom.manifest.entryModule = "ModOom";
    modOom.sources = {{
        "ModOom.rscript",
        R"RS(
module ModOom;

public class HoldRef {
    public int[] chunk;
    public HoldRef prev;
    public HoldRef(int[] c, HoldRef p) {
        chunk = c;
        prev = p;
    }
}

[EventHandler("OnWorldTick")]
public void ExhaustMemory(int step) {
    HoldRef head = null;
    int i = 0;
    while (i < 64) {
        int[] big = new int[2048];
        big[0] = step + i;
        head = new HoldRef(big, head);
        i = i + 1;
    }
}
)RS"}};

    const auto loadReport = host.loadMods({modOom, modInfiniteLoop, modGoodB, modGoodA});
    if (!loadReport.succeeded) {
        for (const auto& d : loadReport.plan.diagnostics) {
            std::cerr << "PLAN ERR: " << d.message << "\n";
        }
        for (const auto& [mid, bag] : loadReport.compileDiagnostics) {
            for (const auto& item : bag.items()) {
                std::cerr << "COMPILE ERR [" << mid << "] " << item.code << ": " << item.message << "\n";
            }
        }
        for (const auto& [mid, lerr] : loadReport.linkErrors) {
            std::cerr << "LINK ERR [" << mid << "]: " << lerr << "\n";
        }
    }
    require(loadReport.succeeded, "ModHost loadMods failed");

    // Verify independent ManagedHeaps across all 4 Mods
    const auto hA = host.modHeap("mod_a_healthy")->heapId();
    const auto hB = host.modHeap("mod_b_restricted")->heapId();
    const auto hC = host.modHeap("mod_c_infinite_loop")->heapId();
    const auto hD = host.modHeap("mod_d_memory_bomb")->heapId();
    require(
        hA != hB && hA != hC && hA != hD && hB != hC && hB != hD && hC != hD,
        "Every loaded Mod MUST own an isolated ManagedHeap with unique heapId");

    // Tick 1: mod_c_infinite_loop hits InstructionBudgetExceeded, mod_d_memory_bomb hits OutOfMemory,
    // while mod_a_healthy and mod_b_restricted succeed 100%!
    const auto tick1 = host.publishEvent("OnWorldTick", {runtime::Value{std::int64_t{5}}});
    require(tick1.deliveredCount == 2 && tick1.failedCount == 2 && tick1.skippedTrippedCount == 0,
        "Tick 1 expected 2 healthy deliveries and 2 isolated sandbox faults");

    require(host.isModActive("mod_a_healthy"), "mod_a_healthy must remain Active");
    require(host.isModActive("mod_b_restricted"), "mod_b_restricted must remain Active");
    require(host.isModTripped("mod_c_infinite_loop"), "mod_c_infinite_loop must be Tripped");
    require(host.isModTripped("mod_d_memory_bomb"), "mod_d_memory_bomb must be Tripped");

    const auto loopStats = host.modStats("mod_c_infinite_loop");
    const auto oomStats = host.modStats("mod_d_memory_bomb");
    require(
        loopStats.has_value() && loopStats->faultCount == 1 &&
            loopStats->lastError.code == runtime::ErrorCode::InstructionBudgetExceeded,
        "mod_c_infinite_loop must record InstructionBudgetExceeded");
    require(
        oomStats.has_value() && oomStats->faultCount == 1 &&
            oomStats->lastError.code == runtime::ErrorCode::OutOfMemory,
        "mod_d_memory_bomb must record OutOfMemory");

    // Tick 2: Tripped Mods are short-circuited immediately without executing instructions!
    const auto tick2 = host.publishEvent("OnWorldTick", {runtime::Value{std::int64_t{3}}});
    require(
        tick2.deliveredCount == 2 && tick2.failedCount == 0 && tick2.skippedTrippedCount == 2,
        "Tick 2 must skip tripped Mods while delivering to healthy Mods");

    // Calling tripped Mod endpoint directly returns ModTripped immediately
    const auto blockedCall = host.invokeModEndpoint("mod_c_infinite_loop", "SpinForever", {runtime::Value{std::int64_t{1}}});
    require(!blockedCall.succeeded && blockedCall.status == plugin::BusCallStatus::ModTripped,
        "Tripped Mod endpoint must return BusCallStatus::ModTripped");

    // Healthy Mod A & Mod B states are 100% intact (ticks = 5 + 3 = 8)
    const auto statusA = host.invokeModEndpoint("mod_a_healthy", "QueryStatus");
    require(
        statusA.succeeded && std::get<std::int64_t>(statusA.value) == 8 + 7777 + 9,
        "Healthy Mod A QueryStatus incorrect: " + statusA.message);

    const auto sumB = host.invokeModEndpoint("mod_b_restricted", "GetSum");
    require(sumB.succeeded && std::get<std::int64_t>(sumB.value) == 80,
        "Healthy Mod B GetSum incorrect");

    // Verify Mod B capability enforcement when attempting unauthorized ReadSecretKey
    host.setFaultThreshold(2); // Allow 1 capability error without tripping immediately
    const auto deniedSecret = host.invokeModEndpoint("mod_b_restricted", "TryReadSecret");
    require(
        !deniedSecret.succeeded &&
            deniedSecret.runtimeError.code == runtime::ErrorCode::CapabilityDenied &&
            host.isModActive("mod_b_restricted"),
        "Unauthorized host capability in Mod B must fail with CapabilityDenied");
}

// ============================================================================
// 6. Zero-Dangling Cross-Heap Deep-Copy Marshaler & Bus Reference Rejection
// ============================================================================
void testCrossHeapMarshalerAndZeroDanglingBus() {
    // 6a. Direct unit tests of `marshalCrossHeapValue` across two independent heaps
    {
        auto heapSrc = std::make_unique<runtime::ManagedHeap>();
        runtime::ManagedHeap heapDst;

        // Scalars & Inline/Arena Math values (Fixed, Fixed16, Vec2, Vec3, Quat)
        const std::vector<runtime::Value> primitivesAndMath = {
            runtime::Value{true},
            runtime::Value{runtime::ByteValue{200}},
            runtime::Value{std::int64_t{-123456}},
            runtime::Value{runtime::LongValue{9876543210LL}},
            runtime::Value{3.1415926535},
            runtime::Value{runtime::EnumValue{semantic::stableTypeId("Shared::Mode"), 2}},
            runtime::Value{runtime::FixedValue{65536 * 5}},
            runtime::Value{runtime::Fixed16Value{1024}},
            runtime::Value{runtime::Vec2Value{100, -200}},
            runtime::Value{runtime::Vec3Value{1, 2, 3}},
            runtime::Value{runtime::QuatValue{10, 20, 30, 40}},           // QuatInline
            runtime::Value{runtime::QuatValue{10, 20, 30, 16'000'000}},   // QuatArena
        };

        for (const auto& v : primitivesAndMath) {
            const auto m = plugin::marshalCrossHeapValue(v, heapSrc.get(), &heapDst);
            require(m.succeeded && m.value == v, "Scalar/Math cross-heap marshal mismatch");
        }

        // Heap-allocated String -> deep-copied onto heapDst, survives destruction of heapSrc!
        const auto srcStrRef = heapSrc->allocateString("isolated_payload_string");
        require(srcStrRef.has_value(), "src string alloc failed");
        auto srcStrRoot = heapSrc->retain(*srcStrRef);

        // Compact int[] packed array -> deep-copied onto heapDst!
        const auto intArrTypeId = semantic::stableTypeId("int[]");
        const auto srcIntArr = heapSrc->allocateTypedArray(
            intArrTypeId, semantic::PrimitiveType::Int, 0, 4, runtime::Value{std::int64_t{0}});
        require(srcIntArr.has_value(), "src int[] alloc failed");
        auto srcArrRoot = heapSrc->retain(*srcIntArr);
        const std::int64_t rawNums[4] = {10, 20, 30, 40};
        require(heapSrc->writePackedIntArray(*srcIntArr, rawNums, 4), "writePackedIntArray failed");

        // Pure value StructValue DTO containing nested string & Vec2
        const auto dtoTypeId = semantic::stableTypeId("Shared::PacketDto");
        const runtime::StructValue srcDto = runtime::makeStructValueFromFields(
            dtoTypeId,
            std::vector<runtime::Value>{
                runtime::Value{std::int64_t{77}},
                runtime::Value{*srcStrRef},
                runtime::Value{runtime::Vec2Value{500, 600}}});

        const auto marshaledStr = plugin::marshalCrossHeapValue(
            runtime::Value{*srcStrRef}, heapSrc.get(), &heapDst);
        const auto marshaledArr = plugin::marshalCrossHeapValue(
            runtime::Value{*srcIntArr}, heapSrc.get(), &heapDst);
        const auto marshaledDto = plugin::marshalCrossHeapValue(
            runtime::Value{srcDto}, heapSrc.get(), &heapDst);

        require(marshaledStr.succeeded && marshaledArr.succeeded && marshaledDto.succeeded,
            "String / Packed int[] / DTO StructValue marshaling must succeed");

        const auto dstStrRef = std::get<runtime::ObjectRef>(marshaledStr.value);
        const auto dstArrRef = std::get<runtime::ObjectRef>(marshaledArr.value);
        require(
            dstStrRef.heapId == static_cast<std::uint32_t>(heapDst.heapId()) &&
                dstArrRef.heapId == static_cast<std::uint32_t>(heapDst.heapId()),
            "Marshaled String and Array MUST reside on destination heap, never sharing source slot");

        auto dstStrRoot = heapDst.retain(dstStrRef);
        auto dstArrRoot = heapDst.retain(dstArrRef);
        auto dstDtoRoot = heapDst.retain(marshaledDto.value);

        // Reject Record ObjectRef, NativeHandle, Struct with Record field, and Object[] array!
        const auto srcRecord = heapSrc->allocateObject(
            semantic::stableTypeId("PrivateMod::Entity"),
            {runtime::Value{std::int64_t{1}}},
            {});
        require(srcRecord.has_value(), "srcRecord alloc failed");
        auto srcRecRoot = heapSrc->retain(*srcRecord);

        const auto rejRecord = plugin::marshalCrossHeapValue(
            runtime::Value{*srcRecord}, heapSrc.get(), &heapDst);
        require(
            !rejRecord.succeeded &&
                rejRecord.errorCode == plugin::MarshalErrorCode::CrossHeapReferenceRejected,
            "Direct Record ObjectRef MUST be rejected with CrossHeapReferenceRejected");

        runtime::NativeHandleRegistry handleReg;
        auto dummyResource = std::make_shared<int>(123);
        const auto nativeHandle = handleReg.create(
            semantic::stableTypeId("Native::Socket"), dummyResource, "sock");
        const auto rejHandle = plugin::marshalCrossHeapValue(
            runtime::Value{nativeHandle}, heapSrc.get(), &heapDst);
        require(
            !rejHandle.succeeded &&
                rejHandle.errorCode == plugin::MarshalErrorCode::CrossHeapReferenceRejected,
            "NativeHandle MUST be rejected with CrossHeapReferenceRejected");

        const runtime::StructValue taintedDto = runtime::makeStructValueFromFields(
            semantic::stableTypeId("Shared::TaintedDto"),
            std::vector<runtime::Value>{runtime::Value{std::int64_t{1}}, runtime::Value{*srcRecord}});
        const auto rejTaintedDto = plugin::marshalCrossHeapValue(
            runtime::Value{taintedDto}, heapSrc.get(), &heapDst);
        require(
            !rejTaintedDto.succeeded &&
                rejTaintedDto.errorCode == plugin::MarshalErrorCode::CrossHeapReferenceRejected,
            "StructValue containing Record ObjectRef MUST be rejected with CrossHeapReferenceRejected");

        const auto srcObjArr = heapSrc->allocateTypedArray(
            semantic::stableTypeId("PrivateMod::Entity[]"),
            semantic::PrimitiveType::Object,
            semantic::stableTypeId("PrivateMod::Entity"),
            1,
            runtime::Value{runtime::NullObject{}});
        require(srcObjArr.has_value(), "srcObjArr alloc failed");
        const auto rejObjArr = plugin::marshalCrossHeapValue(
            runtime::Value{*srcObjArr}, heapSrc.get(), &heapDst);
        require(
            !rejObjArr.succeeded &&
                rejObjArr.errorCode == plugin::MarshalErrorCode::CrossHeapReferenceRejected,
            "Object[] reference array MUST be rejected with CrossHeapReferenceRejected");

        // Now collect AND completely destroy source heap! Verify zero dangling pointers in heapDst!
        srcStrRoot.reset();
        srcArrRoot.reset();
        srcRecRoot.reset();
        heapSrc->collect();
        heapSrc.reset();

        heapDst.collect();
        require(
            heapDst.stringView(dstStrRef) == std::optional<std::string_view>{"isolated_payload_string"},
            "Destination string corrupted after source heap destruction");
        std::vector<std::int64_t> dstNums;
        require(
            heapDst.readPackedIntArray(dstArrRef, dstNums) &&
                dstNums == std::vector<std::int64_t>{10, 20, 30, 40},
            "Destination packed int[] corrupted after source heap destruction");
        const auto& dstDto = std::get<runtime::StructValue>(marshaledDto.value);
        const auto dtoStrField = runtime::structValueFieldAt(dstDto, 1);
        require(
            dtoStrField.has_value() &&
                readHeapOrInlineString(*dtoStrField, &heapDst) == "isolated_payload_string",
            "Destination StructValue string field corrupted after source heap destruction");
    }

    // 6b. End-to-end cross-Mod DTO & array transfer + leaked return ObjectRef rejection via ModHost
    plugin::ModHost host;
    const compiler::SourceFile sharedContracts{
        "Shared.BusContracts.d.rscript",
        R"RS(
module Shared.BusContracts;

public struct TradeOrderDto {
    public int itemId;
    public int quantity;
    public string memo;

    public TradeOrderDto(int id, int qty, string m) {
        itemId = id;
        quantity = qty;
        memo = m;
    }
}
)RS"};
    host.registerHostModule("Shared.BusContracts", sharedContracts);

    plugin::ModPackage producerPkg;
    producerPkg.manifest.id = "producer_mod";
    producerPkg.manifest.version = {1, 0, 0};
    producerPkg.manifest.entryModule = "Mod.Producer";
    producerPkg.sources = {{
        "Producer.rscript",
        R"RS(
module Mod.Producer;
import Shared.BusContracts;

public class SecretInternalEntity {
    public int token = 999;
    public SecretInternalEntity() {}
}

[ModEndpoint("BuildOrder")]
public TradeOrderDto BuildOrder(int id, int qty, string note) {
    return new TradeOrderDto(id, qty, note + "_ok");
}

[ModEndpoint("LeakInternalObject")]
public SecretInternalEntity LeakInternalObject() {
    return new SecretInternalEntity();
}
)RS"}};

    plugin::ModPackage consumerPkg;
    consumerPkg.manifest.id = "consumer_mod";
    consumerPkg.manifest.version = {1, 0, 0};
    consumerPkg.manifest.dependencies = {{"producer_mod", "^1.0.0"}};
    consumerPkg.manifest.entryModule = "Mod.Consumer";
    consumerPkg.sources = {{
        "Consumer.rscript",
        R"RS(
module Mod.Consumer;
import Shared.BusContracts;

public class Ledger {
    public static int totalValue = 0;
    public static string lastMemo = "";
}

[EventHandler("OnOrderConfirmed")]
public void OnOrder(TradeOrderDto order, int[] priceTable) {
    Ledger.totalValue = Ledger.totalValue + order.quantity * priceTable[order.itemId];
    Ledger.lastMemo = order.memo;
}

[ModEndpoint("GetTotal")]
public int GetTotal() {
    return Ledger.totalValue;
}

[ModEndpoint("GetLastMemo")]
public string GetLastMemo() {
    return Ledger.lastMemo;
}
)RS"}};

    const auto loadRep = host.loadMods({consumerPkg, producerPkg});
    require(loadRep.succeeded, "Producer/Consumer Mod load failed");

    // Build DTO in producer_mod, marshal back to hostHeap
    const auto orderRes = host.invokeModEndpoint(
        "producer_mod",
        "BuildOrder",
        {runtime::Value{std::int64_t{1}}, runtime::Value{std::int64_t{3}}, runtime::Value{"batchA"}});
    require(orderRes.succeeded, "BuildOrder endpoint failed: " + orderRes.message);
    auto hostOrderRoot = host.hostHeap().retain(orderRes.value);

    // Force full GC in producer_mod's heap before forwarding DTO to consumer_mod!
    host.modHeap("producer_mod")->collect();

    // Build compact int[] price table on hostHeap and publish event with (TradeOrderDto, int[])
    const auto priceArr = host.hostHeap().allocateTypedArray(
        semantic::stableTypeId("int[]"),
        semantic::PrimitiveType::Int,
        0,
        3,
        runtime::Value{std::int64_t{0}});
    require(priceArr.has_value(), "host priceArr alloc failed");
    auto priceRoot = host.hostHeap().retain(*priceArr);
    const std::int64_t prices[3] = {10, 25, 50};
    require(host.hostHeap().writePackedIntArray(*priceArr, prices, 3), "write prices failed");

    const auto pubRep = host.publishEvent(
        "OnOrderConfirmed",
        {orderRes.value, runtime::Value{*priceArr}},
        &host.hostHeap());
    require(pubRep.succeeded && pubRep.deliveredCount == 1 && pubRep.failedCount == 0,
        "publishEvent with DTO + int[] failed");

    const auto totalRes = host.invokeModEndpoint("consumer_mod", "GetTotal");
    const auto memoRes = host.invokeModEndpoint("consumer_mod", "GetLastMemo");
    require(totalRes.succeeded && std::get<std::int64_t>(totalRes.value) == 75,
        "Consumer totalValue mismatch (expected 3 * 25 = 75)");
    require(
        memoRes.succeeded &&
            readHeapOrInlineString(memoRes.value, &host.hostHeap()) == "batchA_ok",
        "Consumer lastMemo mismatch");

    // Verify malicious endpoint attempting to return an internal Record ObjectRef is rejected!
    const auto leakAttempt = host.invokeModEndpoint("producer_mod", "LeakInternalObject");
    require(
        !leakAttempt.succeeded &&
            leakAttempt.status == plugin::BusCallStatus::CrossHeapReferenceRejected &&
            leakAttempt.marshalError == plugin::MarshalErrorCode::CrossHeapReferenceRejected,
        "Endpoint returning internal Record ObjectRef MUST be blocked with CrossHeapReferenceRejected");

    // Verify publishEvent attempting to broadcast a Record ObjectRef is rejected!
    runtime::RuntimeError allocErr;
    const auto hostRec = host.modReflection("producer_mod")->instantiateUninitializedObject(
        *host.modHeap("producer_mod"), "SecretInternalEntity", allocErr);
    require(hostRec.has_value(), "instantiate SecretInternalEntity failed");
    auto hostRecRoot = host.modHeap("producer_mod")->retain(*hostRec);
    const auto pubLeak = host.publishEvent(
        "OnOrderConfirmed",
        {runtime::Value{*hostRec}},
        host.modHeap("producer_mod").get());
    require(
        !pubLeak.succeeded &&
            pubLeak.marshalError == plugin::MarshalErrorCode::CrossHeapReferenceRejected,
        "publishEvent with Record ObjectRef MUST be rejected with CrossHeapReferenceRejected");
}

} // namespace

int main() {
    try {
        std::cout << "[1/6] Testing deterministic ReflectionRegistry metadata & attribute queries...\n";
        testReflectionMetadataAndAttributeQueries();

        std::cout << "[2/6] Testing dynamic instantiation, ObjectFastView safety & polymorphic dispatch...\n";
        testDynamicOperationsAndFastViewSafety();

        std::cout << "[3/6] Testing RealScript.Reflect capability gate & strict determinism...\n";
        testScriptReflectCapabilityAndDeterminism();

        std::cout << "[4/6] Testing SemVer constraints, JSON manifest & lexicographical Kahn solver...\n";
        testSemVerAndTopologicalDependencySolver();

        std::cout << "[5/6] Testing ModHost AST security hooks, OOM quota & infinite-loop circuit breaker...\n";
        testModHostSandboxIsolationAndCompilerPasses();

        std::cout << "[6/6] Testing cross-heap deep-copy marshaler & zero-dangling event/service bus...\n";
        testCrossHeapMarshalerAndZeroDanglingBus();

        std::cout << "ALL P3 Stage 4 Reflection & ModHost Sandbox tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "TEST FAILURE: " << ex.what() << "\n";
        return 1;
    }
}
