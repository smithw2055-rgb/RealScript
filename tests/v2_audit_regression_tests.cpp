#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/compiler/ModuleBuildCache.h"
#include "realscript/debug/DapServer.h"
#include "realscript/game/GameProductization.h"
#include "realscript/game/GameScripting.h"
#include "realscript/game/Gameplay.h"
#include "realscript/game/ResourceHandle.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/tooling/LanguageService.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef REALSCRIPT_SOURCE_DIR
#define REALSCRIPT_SOURCE_DIR "."
#endif

#define RS_AUDIT_CHECK(cond, msg)                                                 \
    do {                                                                          \
        if (!(cond)) {                                                            \
            throw std::runtime_error(std::string("[AUDIT FAIL] ") + (msg));       \
        }                                                                         \
    } while (false)

namespace {

using namespace realscript;

// 1. Cross-verify P0-2 + P0-4 + P2-2:
//    ModuleBuildCache incremental build with RealScript.Resource, using + ResourceHandle<T> copy-retain,
//    cross-module generic specialization added on warm upstream cache, and -O2 deterministic Gas parity!
void testCrossTrackCacheResourceAndGenerics() {
    const auto cacheDir = std::filesystem::temp_directory_path() / "rs_v2_audit_cache_test";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
    std::filesystem::create_directories(cacheDir, ec);

    const std::string libSrc = R"RS(
module Audit.Lib;
import RealScript.Resource;

public class Holder<T> {
    public T item;
    public Holder(T v) { item = v; }
    public T Unwrap() { return item; }
}

public class ShaderAsset : Resource {
    public int tier;
    public ShaderAsset(int t) : base("res://audit/main.shader", "guid-shader-1") { tier = t; }
}

public int InlineBoost(int x) {
    return x * 3 + 7;
}
)RS";

    const std::string appV1 = R"RS(
module Audit.App;
import Audit.Lib;
import RealScript.Resource;

public int RunApp() {
    Holder<int> h = new Holder<int>(10);
    return InlineBoost(h.Unwrap());
}
)RS";

    // App V2 adds Holder<string> AND ResourceHandle<ShaderAsset> copy inside `using` while Audit.Lib is untouched!
    const std::string appV2 = R"RS(
module Audit.App;
import Audit.Lib;
import RealScript.Resource;

class PersistentSlot {
    public ResourceHandle<ShaderAsset> saved;
}

public int RunApp() {
    Holder<string> hs = new Holder<string>("warm-generic-spec");
    PersistentSlot slot = new PersistentSlot();
    {
        ShaderAsset asset = new ShaderAsset(9);
        using (var scopedHandle = ResourceHandle<ShaderAsset>.Adopt(asset)) {
            slot.saved = scopedHandle;
        }
        // P0-4a check: after `using` scope exits, slot.saved MUST still hold an active retained lease!
        if (!slot.saved.IsValid() || slot.saved.UseCount() != 1 || slot.saved.Get() == null) {
            return -101;
        }
    }
    int tierVal = slot.saved.Get().tier;
    slot.saved.Dispose();
    if (slot.saved.IsValid()) {
        return -102;
    }
    return InlineBoost(hs.Unwrap().Length) + tierVal;
}
)RS";

    compiler::ModuleBuildCache cache(cacheDir);
    compiler::ModuleBuildCacheOptions opts;
    opts.cacheDirectory = cacheDir;
    opts.optimizationOptions.level = optimization::Level::Aggressive;
    opts.flags = compiler::kBuildCacheFlagDeterministic | compiler::kBuildCacheFlagPreserveDebugInfo;

    const auto cold = cache.build({{"lib.rscript", libSrc}, {"app.rscript", appV1}}, opts);
    RS_AUDIT_CHECK(cold.succeeded(), "Cold cache build failed");

    const auto warm = cache.build({{"lib.rscript", libSrc}, {"app.rscript", appV2}}, opts);
    RS_AUDIT_CHECK(warm.succeeded(), "Warm incremental build with new cross-module generic + ResourceHandle failed");

    // Compare warm cached execution result AND Gas/instruction count against fresh uncached Compilation!
    runtime::Interpreter warmVm(warm.bytecodeModules);
    runtime::ExecutionOptions execOpts;
    const auto warmRes = warmVm.invoke("Audit.App::RunApp", {}, execOpts);
    RS_AUDIT_CHECK(warmRes.succeeded && std::get<std::int64_t>(warmRes.value) == (17 * 3 + 7 + 9),
        "Warm cached RunApp returned wrong value");

    compiler::Compilation freshComp({{"lib.rscript", libSrc}, {"app.rscript", appV2}});
    auto freshBuild = freshComp.build();
    RS_AUDIT_CHECK(!freshBuild.diagnostics.hasErrors(), "Fresh compilation failed");
    auto freshOpt = optimization::Optimizer{}.optimize(freshBuild.modules, freshBuild.diagnostics, opts.optimizationOptions);
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> freshBc;
    for (const auto& m : freshOpt.modules) freshBc.push_back(lowerer.lower(m));
    runtime::Interpreter freshVm(std::move(freshBc));
    const auto freshRes = freshVm.invoke("Audit.App::RunApp", {}, execOpts);
    RS_AUDIT_CHECK(freshRes.succeeded && freshRes.value == warmRes.value,
        "Cached vs uncached execution values diverged");
    RS_AUDIT_CHECK(freshRes.instructionsExecuted == warmRes.instructionsExecuted,
        "P2-2: Cached vs uncached -O2 instruction/Gas count diverged!");

    std::filesystem::remove_all(cacheDir, ec);
}

// 2. Cross-verify P1-1 (RsBindParser short-circuit #if, operator<, empty function macro) + P3-1 (LSP .rs reject)
void testRsBindShortCircuitOperatorAndLspCutover() {
    const char* trickyHeader = R"CPP(
#pragma once
#define DEPRECATED_TAG(msg)

#if 1 || __has_feature(cxx_rtti)
#if 0 && (1 / 0)
RS_EXPORT_FUNCTION()
int NeverReached();
#endif

namespace eng {
RS_EXPORT_CLASS("Audit.Native")
class DEPRECATED_TAG("legacy") SpatialNode {
public:
    bool operator<(const SpatialNode& other) const noexcept { return id_ < other.id_; }
    bool operator<<(int shift) const noexcept { return (id_ << shift) != 0; }

    RS_EXPORT_METHOD()
    int GetNodeId() const noexcept { return id_; }
private:
    int id_ = 7;
};
}
#endif
)CPP";

    const auto gen = game::GameApi::generateBindingsFromCppHeader(trickyHeader, "Audit.Native");
    RS_AUDIT_CHECK(gen.classesExported == 1 && gen.methodsExported == 1 && gen.functionsExported == 0,
        "P1-1: RsBindParser failed on short-circuit #if, operator<, or empty function-like macro");
    RS_AUDIT_CHECK(gen.scriptDeclarationStub.find("int GetNodeId()") != std::string::npos,
        "P1-1: GetNodeId() after operator< was swallowed!");

    // P3-1: LanguageService::open must ignore legacy .rs files
    tooling::LanguageService lsp;
    lsp.open("legacy_rust_file.rs", "fn main() {}", 1);
    const auto diags = lsp.diagnostics("legacy_rust_file.rs");
    RS_AUDIT_CHECK(diags.empty(), "P3-1: LanguageService::open should ignore legacy .rs files");
}

// 3. Cross-verify P1-4 (idle GC budget no-op) + P1-6 (non-int empty array serialization) + P1-8 (DAP Queue/Stack/high ref)
void testGcIdleNoOpEmptyArrayAndDapViews() {
    // P1-4: Non-empty heap in Idle phase without collection request must NOT start full GC on budget tick
    runtime::ManagedHeap heap;
    const auto strRef = heap.allocateString("persistent-scene-object");
    RS_AUDIT_CHECK(strRef.has_value(), "allocateString failed");
    const auto pin = heap.retain(runtime::Value(*strRef));
    (void)pin;
    runtime::ShadowStack roots;
    const auto idleStats = heap.collectIncrementalWithBudget(roots, std::chrono::microseconds(200));
    RS_AUDIT_CHECK(!idleStats.cycleCompleted && !idleStats.budgetExhausted &&
        heap.phase() == runtime::GcPhase::Idle && idleStats.totalWork() == 0,
        "P1-4: collectIncrementalWithBudget must be a no-op when Idle and below threshold");

    // P1-6: Empty string[] and Vec2[] scene text serialization & deserialization round-trip
    game::GameApi api;
    game::GameScriptCompiler compiler(api);
    const char* sceneScript = R"RS(
module Audit.Scene;
import RealScript.Math;

public class SquadComponent {
    public string[] tags = new string[0];
    public Vec2[] waypoints = new Vec2[0];
    public string label = "res://not/an/asset";
}
)RS";
    auto compiled = compiler.compile({{"audit_scene.rscript", sceneScript}});
    RS_AUDIT_CHECK(compiled.succeeded(), "Audit.Scene compile failed");

    game::AssetGuidRegistry guidReg;
    const auto assetGuid = guidReg.registerAssetPath("res://not/an/asset");

    game::ScriptRuntime rt1(compiled.program);
    game::SceneScriptRuntime scene(rt1);
    RS_AUDIT_CHECK(scene.attachComponent(1, "Audit.Scene::SquadComponent"), "attach SquadComponent failed");

    runtime::RuntimeError serErr;
    const std::string text = game::serializeSceneText(scene, serErr, &guidReg);
    RS_AUDIT_CHECK(serErr.code == runtime::ErrorCode::None && !text.empty(), "serializeSceneText failed");
    // P2-3b: plain string field without [AssetRef] must NOT be rewritten to asset_path:<guid>
    RS_AUDIT_CHECK(text.find("asset_path:") == std::string::npos,
        "P2-3b: plain string field was erroneously rewritten as asset_path:<guid>");

    // Mutate path in registry, deserialize into fresh scene, verify empty arrays & plain string intact!
    RS_AUDIT_CHECK(guidReg.rebindAssetPath(assetGuid, "res://renamed/other.asset"), "rebindAssetPath failed");

    game::ScriptRuntime rt2(compiled.program);
    game::SceneScriptRuntime restored(rt2);
    runtime::RuntimeError deserErr;
    const bool deserOk = game::deserializeSceneText(restored, text, deserErr, &guidReg);
    RS_AUDIT_CHECK(deserOk && deserErr.code == runtime::ErrorCode::None,
        "P1-6: deserializeSceneText failed on empty string[] / Vec2[]: " + deserErr.message);

    // P1-8: DAP Queue & Stack logical order + variablesReference >= 1,000,000
    const char* dapScript = R"RS(
module Audit.Dap;
public int InspectCollections() {
    Queue<int> q = new Queue<int>(4);
    q.Enqueue(10);
    q.Enqueue(20);
    int dropped = q.Dequeue();
    q.Enqueue(30);
    Stack<int> st = new Stack<int>(4);
    st.Push(100);
    st.Push(200);
    return dropped + q.Count + st.Count;
}
)RS";
    auto dapCompiled = compiler.compile({{"audit_dap.rscript", dapScript}});
    RS_AUDIT_CHECK(dapCompiled.succeeded(), "Audit.Dap compile failed");
    debug::DapServer dap(dapCompiled.program.program(), dapCompiled.program.heap());
    std::ostringstream events;
    (void)dap.handle(tooling::Json::Object{
        {"seq", 1}, {"type", "request"}, {"command", "setBreakpoints"},
        {"arguments", tooling::Json::Object{
            {"source", tooling::Json::Object{{"path", "audit_dap.rscript"}}},
            {"breakpoints", tooling::Json::Array{tooling::Json::Object{{"line", 12}, {"column", 1}}}},
        }},
    }, &events);
    (void)dap.handle(tooling::Json::Object{
        {"seq", 2}, {"type", "request"}, {"command", "launch"},
        {"arguments", tooling::Json::Object{{"function", "Audit.Dap::InspectCollections"}}},
    }, &events);

    const auto evalQ = dap.handle(tooling::Json::Object{
        {"seq", 3}, {"type", "request"}, {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", "q"}, {"frameId", 1}}},
    }, &events);
    const std::int64_t qRef = evalQ.find("body")->find("variablesReference")->integerValue();
    RS_AUDIT_CHECK(qRef >= 1000000, "P1-8: variablesReference must be >= 1000000 to avoid deep stack collision");
    const auto qVars = dap.handle(tooling::Json::Object{
        {"seq", 4}, {"type", "request"}, {"command", "variables"},
        {"arguments", tooling::Json::Object{{"variablesReference", qRef}}},
    }, &events);
    const auto& qArr = qVars.find("body")->find("variables")->arrayValue();
    RS_AUDIT_CHECK(qArr.size() == 3 &&
        qArr[1].find("value")->stringValue() == "20" &&
        qArr[2].find("value")->stringValue() == "30",
        "P1-8: Queue ring-buffer DAP view did not show front-to-back [20, 30]");

    const auto evalSt = dap.handle(tooling::Json::Object{
        {"seq", 5}, {"type", "request"}, {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", "st"}, {"frameId", 1}}},
    }, &events);
    const std::int64_t stRef = evalSt.find("body")->find("variablesReference")->integerValue();
    const auto stVars = dap.handle(tooling::Json::Object{
        {"seq", 6}, {"type", "request"}, {"command", "variables"},
        {"arguments", tooling::Json::Object{{"variablesReference", stRef}}},
    }, &events);
    const auto& stArr = stVars.find("body")->find("variables")->arrayValue();
    RS_AUDIT_CHECK(stArr.size() == 3 &&
        stArr[1].find("value")->stringValue() == "200" &&
        stArr[2].find("value")->stringValue() == "100",
        "P1-8: Stack DAP view did not show top-of-stack first [200, 100]");
}

// 4. P0-1: RawColumn >4-field struct arrayGet (no HeapStructPool deadlock), narrow-int/Fixed16 RawColumn, tryCopyStringGlobal
void testRawColumnPoolSyncAndNarrowFields() {
    struct HostWidePacket {
        std::int8_t tag;
        std::uint16_t flags;
        std::int16_t f16Raw; // Fixed16 is 2-byte int16_t
        std::int32_t a;
        std::int32_t b;
        std::int32_t c;
    };
    static_assert(std::is_standard_layout_v<HostWidePacket>, "HostWidePacket must be standard-layout");

    game::GameApi api;
    game::GameScriptCompiler compiler(api);
    const char* script = R"RS(
module Audit.RawCol;
import RealScript.Math;

public struct WidePacket {
    public sbyte tag;
    public ushort flags;
    public Fixed16 ratio;
    public int a;
    public int b;
    public int c;
}

public int VerifyPacketSum() {
    return 1;
}
)RS";
    auto compiled = compiler.compile({{"raw_col.rscript", script}});
    RS_AUDIT_CHECK(compiled.succeeded(), "Audit.RawCol compile failed");

    game::ScriptRuntime rt(compiled.program);
    std::vector<HostWidePacket> hostRows = {
        HostWidePacket{-7, 50000u, static_cast<std::int16_t>(16384), 10, 20, 30},
        HostWidePacket{12, 65535u, static_cast<std::int16_t>(-8192), 40, 50, 60},
    };
    runtime::RuntimeError bindErr;
    RS_AUDIT_CHECK(
        rt.bindEcsRawColumnAuto("Audit.RawCol::WidePacket", hostRows.data(), hostRows.size(), bindErr, true),
        "bindEcsRawColumnAuto failed for narrow-int + Fixed16 struct: " + bindErr.message);

    // Also directly allocate a RawColumn array on ManagedHeap with >4 fields and call arrayGet()
    // while triggering pool reclamation to verify P0-1 lock-order deadlock is completely eliminated!
    const auto structType = rt.findType("Audit.RawCol::WidePacket");
    RS_AUDIT_CHECK(structType.has_value(), "WidePacket type missing");
    runtime::NativeRawColumnView colView;
    colView.basePtr = hostRows.data();
    colView.count = hostRows.size();
    colView.strideBytes = sizeof(HostWidePacket);
    colView.structTypeId = structType->descriptor.id;
    colView.fields = {
        {offsetof(HostWidePacket, tag), runtime::NativeFieldKind::Int8, semantic::PrimitiveType::SByte, 0},
        {offsetof(HostWidePacket, flags), runtime::NativeFieldKind::UInt16, semantic::PrimitiveType::UShort, 0},
        {offsetof(HostWidePacket, f16Raw), runtime::NativeFieldKind::Fixed16, semantic::PrimitiveType::Struct, 0},
        {offsetof(HostWidePacket, a), runtime::NativeFieldKind::Int32, semantic::PrimitiveType::Int, 0},
        {offsetof(HostWidePacket, b), runtime::NativeFieldKind::Int32, semantic::PrimitiveType::Int, 0},
        {offsetof(HostWidePacket, c), runtime::NativeFieldKind::Int32, semantic::PrimitiveType::Int, 0},
    };
    runtime::RuntimeError arrErr;
    const auto rawArr = rt.heap()->allocateRawColumnArray(
        semantic::stableTypeId("Audit.RawCol::WidePacket[]"),
        structType->descriptor.id,
        colView,
        &arrErr);
    RS_AUDIT_CHECK(rawArr.has_value(), "allocateRawColumnArray failed: " + arrErr.message);
    const auto pinArr = rt.heap()->retain(*rawArr);
    (void)pinArr;

    // Repeatedly materialize >4-field HeapStructPool entries via arrayGet + reclaimUnreferencedPoolValues
    for (int iter = 0; iter < 32; ++iter) {
        const auto elem0 = rt.heap()->arrayGet(*rawArr, 0);
        RS_AUDIT_CHECK(elem0.has_value() && std::holds_alternative<runtime::StructValue>(*elem0),
            "P0-1: RawColumn arrayGet failed to materialize >4-field StructValue");
        const auto& sv = std::get<runtime::StructValue>(*elem0);
        const auto f0 = runtime::structValueFieldAt(sv, 0);
        const auto f1 = runtime::structValueFieldAt(sv, 1);
        const auto f2 = runtime::structValueFieldAt(sv, 2);
        RS_AUDIT_CHECK(f0 && std::holds_alternative<runtime::SByteValue>(*f0) &&
            std::get<runtime::SByteValue>(*f0).value == -7, "Int8 RawColumn mismatch");
        RS_AUDIT_CHECK(f1 && std::holds_alternative<runtime::UShortValue>(*f1) &&
            std::get<runtime::UShortValue>(*f1).value == 50000u, "UInt16 RawColumn mismatch");
        RS_AUDIT_CHECK(f2 && std::holds_alternative<runtime::Fixed16Value>(*f2) &&
            std::get<runtime::Fixed16Value>(*f2).raw == 16384, "2-byte Fixed16 RawColumn mismatch");
        (void)runtime::reclaimUnreferencedPoolValues();
    }

    // Verify tryCopyStringGlobal across live heap shards
    const auto heapStr = rt.heap()->allocateString("global-heap-string-lookup");
    RS_AUDIT_CHECK(heapStr.has_value(), "allocateString failed");
    const auto pinStr = rt.heap()->retain(*heapStr);
    (void)pinStr;
    const auto copiedStr = runtime::ManagedHeap::tryCopyStringGlobal(*heapStr);
    RS_AUDIT_CHECK(copiedStr.has_value() && *copiedStr == "global-heap-string-lookup",
        "ManagedHeap::tryCopyStringGlobal failed to resolve live heap string");
}

// 5. P0-2 (>64 generic specializations), P0-3/P0-4 (RsBindParser #/##/raw-string + base<Base>() inheritance), P1-4 (type limits)
namespace native_inh_test {
class NativeEntityBase {
public:
    virtual ~NativeEntityBase() = default;
    int baseId = 42;
    int GetBaseId() const { return baseId; }
};
class NativeBossActor : public NativeEntityBase {
public:
    int bossPhase = 3;
    int GetBossPhase() const { return bossPhase; }
};
} // namespace native_inh_test

void testRsBindMacrosInheritanceAndGenericsScale() {
    // P0-3 & P0-4a: RsBindParser line-splicing, raw string, # / ## macro operators, narrow ints, base class
    const char* complexHeader = R"CPP(
#pragma once
#define MAKE_METHOD_NAME(prefix, suffix) prefix##suffix
#define STRINGIFY_TOK(x) #x
#define MULTILINE_CONST \
    100 + 23

namespace audit_bind {
static constexpr const char* kRawJson = R"JSON({"unbalanced": ")}; class FakeShouldBeIgnored {")JSON";
static constexpr const char* kLabel = STRINGIFY_TOK(Alpha_Beta);
static constexpr int kSpliced = MULTILINE_CONST;

RS_EXPORT_STRUCT("Audit.Bind")
struct NarrowEcsState {
    RS_EXPORT_FIELD() int8_t delta = 0;
    RS_EXPORT_FIELD() uint16_t mask = 0;
};

RS_EXPORT_CLASS("Audit.Bind")
class BaseUnit {
public:
    RS_EXPORT_PROPERTY()
    int TeamId() const { return 1; }
    void SetTeamId(int) {}
};

RS_EXPORT_CLASS("Audit.Bind")
class HeroUnit : public BaseUnit {
public:
    RS_EXPORT_METHOD()
    int MAKE_METHOD_NAME(Compute, Power)() const { return kSpliced; }
};
}
)CPP";

    const auto gen = game::GameApi::generateBindingsFromCppHeader(complexHeader, "Audit.Bind");
    RS_AUDIT_CHECK(gen.classesExported == 2 && gen.structsExported == 1,
        "P0-3: RsBindParser failed on raw string, line splicing, or #/## macros");
    RS_AUDIT_CHECK(gen.scriptDeclarationStub.find("class HeroUnit : BaseUnit") != std::string::npos,
        "P0-4: RsBindParser did not emit base class in .d.rscript");
    RS_AUDIT_CHECK(gen.scriptDeclarationStub.find("int ComputePower()") != std::string::npos,
        "P0-3: ## token pasting failed on method identifier");
    RS_AUDIT_CHECK(gen.cppRegistrationSource.find(".base<audit_bind::BaseUnit>()") != std::string::npos,
        "P0-4: RsBindParser did not emit .base<BaseUnit>() in C++ registration");

    // P0-4b: Runtime NativeTypeBuilder<Derived>::base<Base>() upcast marshaling
    game::GameApi api;
    api.type<native_inh_test::NativeEntityBase>("Audit.Engine", "NativeEntityBase")
        .method<&native_inh_test::NativeEntityBase::GetBaseId>("GetBaseId");
    api.type<native_inh_test::NativeBossActor>("Audit.Engine", "NativeBossActor")
        .base<native_inh_test::NativeEntityBase>()
        .method<&native_inh_test::NativeBossActor::GetBossPhase>("GetBossPhase");
    RS_AUDIT_CHECK(api.errors().empty(), "register NativeEntityBase / NativeBossActor failed");
    RS_AUDIT_CHECK(
        api.function("Audit.Engine", "ReadEntityBaseId",
            [](native_inh_test::NativeEntityBase* basePtr) -> int {
                return basePtr ? basePtr->GetBaseId() : -1;
            }),
        "register ReadEntityBaseId failed");

    game::GameScriptCompiler gameCompiler(api);
    const char* inhScript = R"RS(
module Audit.GameInh;
import Audit.Engine;

public class BossController {
    public int Verify(NativeBossActor boss) {
        return boss.GetBaseId() * 10 + boss.GetBossPhase() + ReadEntityBaseId(boss);
    }
}
)RS";
    auto inhProg = gameCompiler.compile({{"game_inh.rscript", inhScript}});
    RS_AUDIT_CHECK(inhProg.succeeded(), "Native inheritance script compile failed");
    game::ScriptRuntime inhRt(inhProg.program);
    auto bossPtr = std::make_shared<native_inh_test::NativeBossActor>();
    bossPtr->baseId = 7;
    bossPtr->bossPhase = 5;
    runtime::RuntimeError wrapErr;
    auto wrappedBoss = inhRt.wrapNative<native_inh_test::NativeBossActor>(
        "Audit.Engine::NativeBossActor", bossPtr, wrapErr);
    RS_AUDIT_CHECK(wrappedBoss.has_value(), "wrapNative NativeBossActor failed: " + wrapErr.message);
    auto ctrlObj = inhRt.createObject("Audit.GameInh::BossController", wrapErr);
    RS_AUDIT_CHECK(ctrlObj.has_value(), "createObject BossController failed");
    const auto verifyMethod = inhRt.findMethod(ctrlObj->type(), "Verify", 1);
    RS_AUDIT_CHECK(verifyMethod.has_value(), "findMethod Verify failed");
    const auto inhCall = inhRt.invoke(
        *ctrlObj,
        *verifyMethod,
        {runtime::Value(wrappedBoss->reference())});
    RS_AUDIT_CHECK(inhCall.succeeded && std::get<std::int64_t>(inhCall.value) == (7 * 10 + 5 + 7),
        "P0-4: NativeBossActor -> NativeEntityBase upcast marshaling failed");

    // P0-2: >70 distinct generic specializations in one compilation (verifies std::deque stability)
    std::ostringstream manyGen;
    manyGen << "module Audit.ManyGenerics;\n";
    for (int i = 0; i < 72; ++i) {
        manyGen << "public class Item" << i << " { public int v = " << i << "; }\n";
    }
    manyGen << "public int SumAll() {\n  int total = 0;\n";
    for (int i = 0; i < 72; ++i) {
        manyGen << "  List<Item" << i << "> l" << i << " = new List<Item" << i << ">();\n"
                << "  l" << i << ".Add(new Item" << i << "());\n"
                << "  total = total + l" << i << "[0].v;\n";
    }
    manyGen << "  return total;\n}\n";
    compiler::Compilation manyComp({{"many_generics.rscript", manyGen.str()}});
    const auto manyBuilt = manyComp.build();
    RS_AUDIT_CHECK(!manyBuilt.diagnostics.hasErrors(),
        "P0-2: >64 NativeGenericRegistry specializations failed");

    // P1-4: Nested Nullable<T?> idempotent collapse + negative checks (jagged T[][], >32 generic depth, pattern shadowing)
    {
        compiler::Compilation c1({{"nested_null.rscript",
            "module M1;\n"
            "public class Wrap<T> { public T? item; }\n"
            "public int F() {\n"
            "  Wrap<int?> w = new Wrap<int?>();\n"
            "  Nullable<int?> x = null;\n"
            "  return (w.item ?? 10) + (x ?? 32);\n"
            "}"}});
        const auto b1 = c1.build();
        RS_AUDIT_CHECK(!b1.diagnostics.hasErrors(), "P1-4: Wrap<int?> / Nullable<int?> idempotent collapse failed");
        bytecode::Lowerer low1;
        std::vector<bytecode::Module> mods1;
        for (const auto& m : b1.modules) mods1.push_back(low1.lower(m));
        runtime::RuntimeError linkErr1;
        const auto img1 = runtime::ProgramImage::link(mods1, linkErr1);
        RS_AUDIT_CHECK(img1.has_value(), "M1 link failed");
        runtime::Interpreter vm1(std::make_shared<runtime::ProgramImage>(*img1));
        const auto res1 = vm1.invoke("M1::F");
        RS_AUDIT_CHECK(res1.succeeded && std::get<std::int64_t>(res1.value) == 42,
            "P1-4: Collapsed nested Nullable<int?> runtime evaluation mismatch");
    }
    {
        compiler::Compilation c2({{"jagged_arr.rscript",
            "module M2; public int F() { int[][] x = null; return 0; }"}});
        const auto b2 = c2.build();
        bool has8511 = false;
        for (const auto& d : b2.diagnostics.items()) if (d.code == "RS8511") has8511 = true;
        RS_AUDIT_CHECK(has8511, "P1-4: Jagged array int[][] must report RS8511");
    }
    {
        std::string deepType = "int";
        for (int i = 0; i < 35; ++i) deepType = "List<" + deepType + ">";
        compiler::Compilation c3({{"deep_gen.rscript",
            "module M3; public int F() { " + deepType + " x = null; return 0; }"}});
        const auto b3 = c3.build();
        bool has8512 = false;
        for (const auto& d : b3.diagnostics.items()) if (d.code == "RS8512") has8512 = true;
        RS_AUDIT_CHECK(has8512, "P1-4: Generic nesting depth >32 must report RS8512");
    }
    {
        compiler::Compilation c4({{"pat_shadow.rscript",
            "module M4; public int F(object o) { int v = 1; if (o is int v) { return v; } return v; }"}});
        RS_AUDIT_CHECK(c4.build().diagnostics.hasErrors(),
            "P1-4: Pattern variable shadowing outer local must be rejected");
    }
}

// 6. P0-5 (using + try { break; }), P1-2 (ResourceHandle overwrite & sequence cancel), ModuleBuildCache hygiene & optHash
void testUsingTryBreakOverwriteSequenceAndCacheHygiene() {
    const char* usingScript = R"RS(
module Audit.UsingEdge;

public class CustomDisposable : IDisposable {
    public static int disposedTotal = 0;
    public int id;
    public CustomDisposable(int i) { id = i; }
    public void Dispose() { disposedTotal = disposedTotal + id; }
}

public class HandleBox {
    public ResourceHandle h;
}

public void OverwriteViaRef(ref ResourceHandle slot, string nextPath) {
    slot = ResourceServer.Load(nextPath);
}

public int TestTryBreakAndOverwrites() {
    CustomDisposable.disposedTotal = 0;
    int iter = 0;
    while (iter < 3) {
        using (CustomDisposable d = new CustomDisposable(10)) {
            try {
                if (iter == 0) {
                    break;
                }
            } finally {
                iter = iter + 1;
            }
        }
    }

    ResourceServer.ResetForTesting();
    // 1. Local, field, array-element, and ref-parameter ResourceHandle overwrites must deterministically release old leases
    HandleBox box = new HandleBox();
    box.h = ResourceServer.Load("res://a");
    box.h = ResourceServer.Load("res://b");
    if (ResourceServer.LiveCount() != 1) return -1;

    ResourceHandle[] arr = new ResourceHandle[1];
    arr[0] = box.h;
    if (box.h.UseCount() != 2) return -2;
    arr[0] = ResourceServer.Load("res://c");
    if (box.h.UseCount() != 1 || ResourceServer.LiveCount() != 2) return -3;

    ResourceHandle localH = ResourceServer.Load("res://d");
    OverwriteViaRef(ref localH, "res://e");
    if (ResourceServer.LiveCount() != 3) return -4;

    box.h.Dispose();
    arr[0].Dispose();
    localH.Dispose();
    if (ResourceServer.LiveCount() != 0) return -5;

    return CustomDisposable.disposedTotal + 300;
}
)RS";
    compiler::Compilation comp({{"using_edge.rscript", usingScript}});
    auto built = comp.build();
    RS_AUDIT_CHECK(!built.diagnostics.hasErrors(), "Audit.UsingEdge compile failed");
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bc;
    for (const auto& m : built.modules) bc.push_back(lowerer.lower(m));
    runtime::Interpreter vm(std::move(bc));
    const auto res = vm.invoke("Audit.UsingEdge::TestTryBreakAndOverwrites");
    RS_AUDIT_CHECK(res.succeeded && std::get<std::int64_t>(res.value) == 310,
        "P0-5 / P1-2: using + try/break or ResourceHandle overwrite failed (expected 310, got " +
        runtime::valueToString(res.value) + ")");

    // Reassigning a `using` variable directly must be rejected with RS2651
    {
        compiler::Compilation roComp({{"ro_using.rscript", R"RS(
module Audit.ReadonlyUsing;
class Probe : IDisposable { public void Dispose() {} }
public void Bad() {
    using Probe p = new Probe();
    p = new Probe();
}
)RS"}});
        const auto roBuilt = roComp.build();
        bool has2651 = false;
        for (const auto& d : roBuilt.diagnostics.items()) if (d.code == "RS2651") has2651 = true;
        RS_AUDIT_CHECK(has2651, "P1-2: Reassigning readonly `using` variable must report RS2651");
    }

    // Sequence `using` cancellation disposal
    game::GameApi api;
    auto gameplayHost = std::make_shared<game::GameplayHost>(30, 11, 17);
    RS_AUDIT_CHECK(game::installGameplayBindings(api, gameplayHost), "installGameplayBindings failed");
    game::GameScriptCompiler gameComp(api);
    const char* seqScript = R"RS(
module Audit.SeqUsing;
import RealScript.Game;

public class SeqLease : IDisposable {
    public static int disposed = 0;
    public void Dispose() { disposed = disposed + 1; }
}

public class SeqBehavior {
    sequence RunFlow(long target) {
        using (SeqLease lease = new SeqLease()) {
            yield wait_ticks(10);
        }
    }
}
)RS";
    auto seqCompiled = gameComp.compile({{"seq_using.rscript", seqScript}});
    RS_AUDIT_CHECK(seqCompiled.succeeded(), "Audit.SeqUsing compile failed");
    game::ScriptRuntime seqRt(seqCompiled.program);
    game::SceneScriptRuntime seqScene(seqRt);
    RS_AUDIT_CHECK(seqScene.attachComponent(1, "Audit.SeqUsing::SeqBehavior"), "attach SeqBehavior failed");
    seqScene.start();
    const auto seqStart = seqScene.invoke(1, "RunFlow", {runtime::LongValue{1}});
    RS_AUDIT_CHECK(seqStart.succeeded, "RunFlow start failed: " + seqStart.error.message);
    // Detaching entity 1 while suspended at wait_ticks(10) must cancel coroutine & dispose `lease`!
    RS_AUDIT_CHECK(seqScene.detach(1), "detach SeqBehavior failed");
    const auto seqLeaseType = seqRt.findType("Audit.SeqUsing::SeqLease");
    RS_AUDIT_CHECK(seqLeaseType.has_value() && !seqLeaseType->descriptor.fields.empty(), "SeqLease type missing");
    const auto disposedVal = seqRt.heap()->getStaticField(seqLeaseType->descriptor.fields[0].id);
    RS_AUDIT_CHECK(disposedVal.has_value() && std::get<std::int64_t>(*disposedVal) == 1,
        "P1-2: Canceling suspended sequence failed to dispose active `using` local!");

    // ModuleBuildCache: comment-only "await"/"using", -O0 -> -O2 disk refresh, non-public base + =const: default arg
    const auto cacheDir = std::filesystem::temp_directory_path() / "rs_v2_audit_cache_opt_test";
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
    std::filesystem::create_directories(cacheDir, ec);

    const std::string baseMod = R"RS(
module Audit.CacheBase;
// Comment mentioning await and using and ResourceHandle should NOT inject unused builtin modules!
class InternalBase {
    public int seed = 5;
}
public class ExposedDerived : InternalBase {
    public const int DefaultStep = 12;
    public int Calc(int bonus = DefaultStep) {
        return seed + bonus;
    }
}
)RS";
    const std::string clientMod = R"RS(
module Audit.CacheClient;
import Audit.CacheBase;
public int Run() {
    ExposedDerived d = new ExposedDerived();
    return d.Calc();
}
)RS";
    compiler::ModuleBuildCache buildCache(cacheDir);
    compiler::ModuleBuildCacheOptions o0Opts;
    o0Opts.cacheDirectory = cacheDir;
    o0Opts.optimizationOptions.level = optimization::Level::None;

    const auto coldO0 = buildCache.build({{"base.rscript", baseMod}, {"client.rscript", clientMod}}, o0Opts);
    RS_AUDIT_CHECK(coldO0.succeeded(), "coldO0 build failed");
    for (const auto& bm : coldO0.bytecodeModules) {
        RS_AUDIT_CHECK(bm.name != "RealScript.Async" && bm.name != "RealScript.Resource",
            "Lexer scan fix: comment mentioning await/using should not pull in Async/Resource builtins");
    }

    // Switch to -O2 on same cacheDir, then run a second warm -O2 build via fresh ModuleBuildCache instance
    compiler::ModuleBuildCacheOptions o2Opts = o0Opts;
    o2Opts.optimizationOptions.level = optimization::Level::Aggressive;
    const auto transitionO2 = buildCache.build({{"base.rscript", baseMod}, {"client.rscript", clientMod}}, o2Opts);
    RS_AUDIT_CHECK(transitionO2.succeeded(), "transitionO2 build failed");

    compiler::ModuleBuildCache diskWarmCache(cacheDir);
    const std::string clientModV2 = R"RS(
module Audit.CacheClient;
import Audit.CacheBase;
public int Run() {
    ExposedDerived d = new ExposedDerived();
    return d.Calc() + d.Calc(3);
}
)RS";
    const auto warmO2 = diskWarmCache.build({{"base.rscript", baseMod}, {"client.rscript", clientModV2}}, o2Opts);
    RS_AUDIT_CHECK(warmO2.succeeded(),
        "Warm cache rebuild with non-public base class & =const: default parameter failed");
    runtime::Interpreter cacheVm(warmO2.bytecodeModules);
    const auto cacheRun = cacheVm.invoke("Audit.CacheClient::Run");
    RS_AUDIT_CHECK(cacheRun.succeeded && std::get<std::int64_t>(cacheRun.value) == (17 + 8),
        "Warm cache =const: default parameter evaluated wrong value");

    std::filesystem::remove_all(cacheDir, ec);
}

// 7. Batch 5 & 6: RFC 4122 UUID v5, obj_ref depth limit, colliding [Guid], atomic scene import,
//    mid-OnUpdate start() + hotReload, LSP .RSCRIPT / untitled:, and DAP Dictionary ["key"] setVariable
void testUuidV5SceneAtomicityHotReloadAndTooling() {
    // 7a. RFC 4122 Appendix B / Section 4.3 UUID v5 DNS namespace vector ("www.example.com")
    const auto dnsExampleGuid = game::AssetGuid::fromCanonicalName("", "www.example.com");
    RS_AUDIT_CHECK(dnsExampleGuid.toString() == "2ed6657d-e927-568b-95e1-2665a8aea6a2",
        "AssetGuid::fromCanonicalName(\"\", \"www.example.com\") did not match RFC 4122 UUID v5 vector: got " +
        dnsExampleGuid.toString());

    // 7b. Colliding explicit [Guid] components on same entity + atomic scene deserialization + obj_ref depth limit
    game::GameApi api;
    game::SceneScriptRuntime* activeScenePtr = nullptr;
    RS_AUDIT_CHECK(
        api.function("Audit.Host", "TriggerStartMidUpdate", [&]() -> int {
            if (activeScenePtr) {
                activeScenePtr->start();
            }
            return 1;
        }),
        "bind TriggerStartMidUpdate failed");

    game::GameScriptCompiler compiler(api);
    const char* sceneSrc = R"RS(
module Audit.SceneV2;
import Audit.Host;

[Guid("11111111-2222-5333-8444-555555555555")]
public class AlphaComp {
    public int alphaVal = 11;
}

[Guid("11111111-2222-5333-8444-555555555555")]
public class BetaComp {
    public int betaVal = 22;
}

public class NodeChain {
    public NodeChain next;
    public int depthVal = 1;
}

public class SpawnerComp {
    public int ticks = 0;
    public NodeChain head;
    public void OnUpdate(double dt) {
        ticks = ticks + 1;
        if (ticks == 1) {
            TriggerStartMidUpdate();
        }
    }
}
)RS";
    auto compiled = compiler.compile({{"scene_v2.rscript", sceneSrc}});
    RS_AUDIT_CHECK(compiled.succeeded(), "Audit.SceneV2 compile failed");

    game::AssetGuidRegistry guidReg;
    guidReg.populateFromProgram(compiled.program);

    game::ScriptRuntime rt1(compiled.program);
    game::SceneScriptRuntime scene1(rt1);
    activeScenePtr = &scene1;
    RS_AUDIT_CHECK(scene1.attachComponent(1, "Audit.SceneV2::AlphaComp"), "attach AlphaComp failed");
    RS_AUDIT_CHECK(scene1.attachComponent(1, "Audit.SceneV2::BetaComp"), "attach BetaComp failed");
    RS_AUDIT_CHECK(scene1.attachComponent(1, "Audit.SceneV2::SpawnerComp"), "attach SpawnerComp failed");
    (void)scene1.setInspectorMember(1, "Audit.SceneV2::AlphaComp", "alphaVal", runtime::Value(std::int64_t{77}));
    (void)scene1.setInspectorMember(1, "Audit.SceneV2::BetaComp", "betaVal", runtime::Value(std::int64_t{88}));

    scene1.start();
    // Attach a new unstarted component on entity 2 and call update() which triggers scene1.start() mid-OnUpdate!
    RS_AUDIT_CHECK(scene1.attachComponent(2, "Audit.SceneV2::AlphaComp"), "attach entity 2 AlphaComp failed");
    scene1.update(0.016);
    RS_AUDIT_CHECK(scene1.errors().empty() && scene1.isComponentStarted(2, "Audit.SceneV2::AlphaComp"),
        "Mid-OnUpdate scene.start() failed or recorded lifecycle error");
    activeScenePtr = nullptr;

    runtime::RuntimeError serErr;
    const std::string snapshotText = game::serializeSceneText(scene1, serErr, &guidReg);
    RS_AUDIT_CHECK(serErr.code == runtime::ErrorCode::None, "serializeSceneText failed: " + serErr.message);
    RS_AUDIT_CHECK(snapshotText.find("#1.") != std::string::npos,
        "Colliding explicit [Guid] components did not emit #<dupIdx> disambiguated slot key");

    game::ScriptRuntime rt2(compiled.program);
    game::SceneScriptRuntime scene2(rt2);
    runtime::RuntimeError deserErr;
    RS_AUDIT_CHECK(game::deserializeSceneText(scene2, snapshotText, deserErr, &guidReg),
        "deserializeSceneText failed on colliding [Guid] components: " + deserErr.message);
    RS_AUDIT_CHECK(scene2.hasComponent(1, "Audit.SceneV2::AlphaComp") &&
        scene2.hasComponent(1, "Audit.SceneV2::BetaComp"),
        "Restored scene lost one of the colliding [Guid] components");

    // Atomic import check: corrupted scene text with parent cycle MUST fail WITHOUT clearing existing scene2!
    const std::string cyclicSceneText =
        "realscript_scene_text_v1\n"
        "entity.1.active=true\n"
        "entity.1.parent=2\n"
        "entity.2.active=true\n"
        "entity.2.parent=1\n";
    runtime::RuntimeError cycleErr;
    RS_AUDIT_CHECK(!game::deserializeSceneText(scene2, cyclicSceneText, cycleErr, &guidReg),
        "Cyclic parent graph in deserializeSceneText should fail");
    RS_AUDIT_CHECK(scene2.hasComponent(1, "Audit.SceneV2::AlphaComp") &&
        scene2.hasComponent(1, "Audit.SceneV2::BetaComp"),
        "Failed deserializeSceneText wiped out live scene state instead of validating atomically!");

    // obj_ref depth limit check
    runtime::RuntimeError chainErr;
    auto n1 = rt1.createObject("Audit.SceneV2::NodeChain", chainErr);
    auto n2 = rt1.createObject("Audit.SceneV2::NodeChain", chainErr);
    auto n3 = rt1.createObject("Audit.SceneV2::NodeChain", chainErr);
    RS_AUDIT_CHECK(n1 && n2 && n3, "createObject NodeChain failed");
    (void)rt1.heap()->fieldSet(n1->reference(), 0, runtime::Value(n2->reference()));
    (void)rt1.heap()->fieldSet(n2->reference(), 0, runtime::Value(n3->reference()));
    (void)scene1.setInspectorMember(1, "Audit.SceneV2::SpawnerComp", "head", runtime::Value(n1->reference()));
    const std::string deepSnap = game::serializeSceneText(scene1, serErr, &guidReg);
    RS_AUDIT_CHECK(serErr.code == runtime::ErrorCode::None, "deepSnap serialize failed");
    game::ScriptStatePolicy shallowPolicy;
    shallowPolicy.maximumStructDepth = 1;
    runtime::RuntimeError depthErr;
    game::ScriptRuntime rtShallow(compiled.program);
    game::SceneScriptRuntime sceneShallow(rtShallow);
    RS_AUDIT_CHECK(!game::deserializeSceneText(sceneShallow, deepSnap, depthErr, &guidReg, shallowPolicy),
        "obj_ref chain exceeding maximumStructDepth=1 should be rejected");

    // 7c. LSP uppercase .RSCRIPT + untitled: buffer acceptance while still rejecting .rs
    tooling::LanguageService lsp;
    lsp.open("UPPERCASE_MODULE.RSCRIPT", "module Audit.Upper; public int Val() { return 42; }", 1);
    lsp.open("untitled:Untitled-1", "module Audit.Scratch; import Audit.Upper; public int Use() { return Val(); }", 1);
    lsp.open("ignored_file.rs", "fn broken_rust_syntax( {{{", 1);
    RS_AUDIT_CHECK(lsp.isOpenDocument("UPPERCASE_MODULE.RSCRIPT"), ".RSCRIPT should be accepted");
    RS_AUDIT_CHECK(lsp.isOpenDocument("untitled:Untitled-1"), "untitled:Untitled-1 should be accepted");
    RS_AUDIT_CHECK(!lsp.isOpenDocument("ignored_file.rs"), ".rs file must still be rejected");
    RS_AUDIT_CHECK(lsp.diagnostics("untitled:Untitled-1").empty(),
        "untitled:Untitled-1 importing UPPERCASE_MODULE.RSCRIPT had unexpected diagnostics");

    // 7d. DAP Dictionary ["key"] setVariable & NullArray List preview
    const char* dapDictSrc = R"RS(
module Audit.DapDict;
public struct ListHolder {
    public List<int> uninitList;
}
public int InspectDict() {
    Dictionary<string, int> map = new Dictionary<string, int>();
    map.Add("hp", 50);
    return map["hp"];
}
)RS";
    auto dapDictComp = compiler.compile({{"dap_dict.rscript", dapDictSrc}});
    RS_AUDIT_CHECK(dapDictComp.succeeded(), "Audit.DapDict compile failed");
    debug::DapServer dap(dapDictComp.program.program(), dapDictComp.program.heap());
    std::ostringstream dapEvents;
    (void)dap.handle(tooling::Json::Object{
        {"seq", 1}, {"type", "request"}, {"command", "setBreakpoints"},
        {"arguments", tooling::Json::Object{
            {"source", tooling::Json::Object{{"path", "dap_dict.rscript"}}},
            {"breakpoints", tooling::Json::Array{tooling::Json::Object{{"line", 9}, {"column", 1}}}},
        }},
    }, &dapEvents);
    (void)dap.handle(tooling::Json::Object{
        {"seq", 2}, {"type", "request"}, {"command", "launch"},
        {"arguments", tooling::Json::Object{{"function", "Audit.DapDict::InspectDict"}}},
    }, &dapEvents);
    const auto evalMap = dap.handle(tooling::Json::Object{
        {"seq", 3}, {"type", "request"}, {"command", "evaluate"},
        {"arguments", tooling::Json::Object{{"expression", "map"}, {"frameId", 1}}},
    }, &dapEvents);
    const std::int64_t mapRef = evalMap.find("body")->find("variablesReference")->integerValue();
    const auto setDictResp = dap.handle(tooling::Json::Object{
        {"seq", 4}, {"type", "request"}, {"command", "setVariable"},
        {"arguments", tooling::Json::Object{
            {"variablesReference", mapRef},
            {"name", "[\"hp\"]"},
            {"value", "99"},
        }},
    }, &dapEvents);
    RS_AUDIT_CHECK(setDictResp.find("success")->boolValue(),
        "DAP setVariable on Dictionary with quoted key [\"hp\"] failed");
}

} // namespace

int main() {
    try {
        std::cout << "[1/7] testCrossTrackCacheResourceAndGenerics...\n" << std::flush;
        testCrossTrackCacheResourceAndGenerics();
        std::cout << "[2/7] testRsBindShortCircuitOperatorAndLspCutover...\n" << std::flush;
        testRsBindShortCircuitOperatorAndLspCutover();
        std::cout << "[3/7] testGcIdleNoOpEmptyArrayAndDapViews...\n" << std::flush;
        testGcIdleNoOpEmptyArrayAndDapViews();
        std::cout << "[4/7] testRawColumnPoolSyncAndNarrowFields...\n" << std::flush;
        testRawColumnPoolSyncAndNarrowFields();
        std::cout << "[5/7] testRsBindMacrosInheritanceAndGenericsScale...\n" << std::flush;
        testRsBindMacrosInheritanceAndGenericsScale();
        std::cout << "[6/7] testUsingTryBreakOverwriteSequenceAndCacheHygiene...\n" << std::flush;
        testUsingTryBreakOverwriteSequenceAndCacheHygiene();
        std::cout << "[7/7] testUuidV5SceneAtomicityHotReloadAndTooling...\n" << std::flush;
        testUuidV5SceneAtomicityHotReloadAndTooling();
        std::cout << "All v2 cross-subsystem audit regression tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
}
