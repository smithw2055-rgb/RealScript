#include "realscript/aot_cpp/AotCpp.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/BindingMarshal.h"
#include "realscript/game/DeterministicMath.h"
#include "realscript/game/GameProductization.h"
#include "realscript/game/RsBindParser.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/jit/Jit.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/runtime/Runtime.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace realscript;

namespace {

void require(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error("P0 upgrade test failure: " + msg);
    }
}

bool approxEqual(double a, double b, double eps = 1e-4) {
    return std::fabs(a - b) <= eps;
}

struct CompiledBundle {
    std::vector<mir::Module> mirModules;
    std::vector<bytecode::Module> bcModules;
    std::shared_ptr<const runtime::ProgramImage> program;
};

CompiledBundle compileSource(
    const std::string& name,
    const std::string& src,
    optimization::Level optLevel = optimization::Level::Aggressive) {
    compiler::Compilation comp({{name, src}});
    auto build = comp.build();
    if (build.diagnostics.hasErrors()) {
        std::string err = "compile failed:\n";
        for (const auto& d : build.diagnostics.items()) {
            err += d.code + ": " + d.message + "\n";
        }
        throw std::runtime_error(err);
    }
    optimization::Options opts;
    opts.level = optLevel;
    auto opt = optimization::Optimizer{}.optimize(std::move(build.modules), build.diagnostics, opts);
    if (build.diagnostics.hasErrors()) {
        throw std::runtime_error("optimizer failed");
    }
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> bc;
    for (const auto& m : opt.modules) {
        bc.push_back(lowerer.lower(m));
    }
    std::vector<bytecode::Module> bcCopy = bc;
    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(std::move(bcCopy), linkErr);
    require(linked.has_value(), "ProgramImage::link failed: " + linkErr.message);
    return CompiledBundle{
        std::move(opt.modules),
        std::move(bc),
        std::make_shared<const runtime::ProgramImage>(std::move(*linked)),
    };
}

// ----------------------------------------------------------------------------
// 1. 16-Byte Value ABI, Lossless QuatfInline/QuatfArena & SDK Marshal Tests
// ----------------------------------------------------------------------------
void testValueAbiAndMarshal() {
    std::cout << "[1/5] Testing 16B Value ABI, Quatf inline/arena canonicalization & SDK Marshal...\n";
    static_assert(sizeof(runtime::Value) == 16, "sizeof(Value) must remain strictly 16 bytes");
    static_assert(alignof(runtime::Value) == 16, "alignof(Value) must remain strictly 16 bytes");
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::Bool) == 4);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::Int64) == 9);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::Long) == 11);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::Float) == 13);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::Double) == 14);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::ObjectRef) == 27);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::Fixed64) == 28);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::Vec2f) == 29);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::Vec3f) == 30);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::QuatfInline) == 31);
    static_assert(static_cast<std::uint8_t>(runtime::ValueTag::QuatfArena) == 32);

    const runtime::QuatfValue qId{0.0f, -0.0f, 0.25f, 1.0f};
    const runtime::Value vId{qId};
    require(vId.rawTag() == runtime::ValueTag::QuatfInline, "w=1.0f must use QuatfInline");
    runtime::QuatfValue qOut{};
    require(vId.tryGetQuatf(qOut) && qOut == runtime::QuatfValue{0.0f, 0.0f, 0.25f, 1.0f},
        "QuatfInline round-trip failed");
    require(vId == runtime::Value{runtime::QuatfValue{-0.0f, 0.0f, 0.25f, 1.0f}},
        "-0.0f canonicalization in QuatfInline failed");

    const runtime::QuatfValue qNonAligned{0.12345679f, -0.23456789f, 0.34567891f, 0.70710677f};
    const runtime::Value vArena{qNonAligned};
    require(vArena.rawTag() == runtime::ValueTag::QuatfArena, "non-aligned w must use QuatfArena");
    runtime::QuatfValue qArenaOut{};
    require(vArena.tryGetQuatf(qArenaOut), "QuatfArena tryGetQuatf failed");
    require(std::memcmp(&qNonAligned, &qArenaOut, sizeof(qNonAligned)) == 0,
        "QuatfArena bit-exact lossless round-trip failed");
    require(vArena == runtime::Value{qNonAligned}, "QuatfArena Value::operator== failed");

    auto apiState = std::make_shared<game::detail::GameApiState>();
    apiState->heap = std::make_shared<runtime::ManagedHeap>();
    runtime::RuntimeError mErr;
    const game::Fixed64 sdkF64 = game::Fixed64::fromInt(500000) + game::Fixed64::fromDouble(0.25);
    const auto vF64 = game::detail::writeValue(apiState, sdkF64, mErr);
    require(vF64.has_value(), "Marshal Fixed64 writeValue");
    const auto sdkF64Back = game::detail::readValue<game::Fixed64>(apiState, *vF64, mErr);
    require(sdkF64Back.has_value() && *sdkF64Back == sdkF64, "Marshal Fixed64 readValue");

    const game::Vec3f sdkV3{1.5f, -2.25f, 4.75f};
    const auto vV3 = game::detail::writeValue(apiState, sdkV3, mErr);
    require(vV3.has_value(), "Marshal Vec3f writeValue");
    const auto sdkV3Back = game::detail::readValue<game::Vec3f>(apiState, *vV3, mErr);
    require(sdkV3Back.has_value() && *sdkV3Back == sdkV3, "Marshal Vec3f readValue");

    const game::Quatf sdkQ{0.0f, 0.70710677f, 0.0f, 0.70710677f};
    const auto vQ = game::detail::writeValue(apiState, sdkQ, mErr);
    require(vQ.has_value(), "Marshal Quatf writeValue");
    const auto sdkQBack = game::detail::readValue<game::Quatf>(apiState, *vQ, mErr);
    require(sdkQBack.has_value() && *sdkQBack == sdkQ, "Marshal Quatf readValue");
}

// ----------------------------------------------------------------------------
// 2. P0-1 Dual-Track Math Script Execution & Typed C++17 AOT Codegen Verification
// ----------------------------------------------------------------------------
void testDualTrackMathExecutionAndAotCodegen() {
    std::cout << "[2/5] Testing Fixed64 (Q32.32) & Vec2f/Vec3f/Quatf in VM & Typed C++17 AOT...\n";
    constexpr const char* kMathScript = R"RS(
module P0.DualMath;
import RealScript.Math;

public long LargeWorldFixed64Raw() {
    Fixed64 a = Fixed64.Add(Fixed64.FromInt(100000L), Fixed64.Ratio(1L, 2L));
    Fixed64 b = Fixed64.Add(Fixed64.FromInt(20L), Fixed64.Ratio(1L, 4L));
    Fixed64 prod = Fixed64.Mul(a, b);
    Fixed64 root = Fixed64.Sqrt(Fixed64.FromInt(144L));
    Fixed64 total = Fixed64.Add(prod, root);
    long whole = Fixed64.FloorToInt(total);
    Fixed64 frac = Fixed64.Sub(total, Fixed64.FromInt(whole));
    return whole * 1000L + Fixed64.RoundToInt(Fixed64.Mul(frac, Fixed64.FromInt(1000L)));
}

public float FloatVectorAndQuatPipeline() {
    Vec2f uv = Vec2f.Add(Vec2f.Create(3.0f, 4.0f), Vec2f.Zero());
    float len2d = Vec2f.Length(uv);

    Vec3f dir = Vec3f.Create(1.0f, 0.0f, 0.0f);
    Quatf rot = Quatf.Normalized(Quatf.FromAxisAngle(Vec3f.Create(0.0f, 0.0f, 1.0f), 1.5707963f));
    Vec3f rotated = Quatf.Rotate(rot, dir);
    Vec3f blended = Vec3f.Lerp(dir, rotated, 0.5f);
    return len2d + rotated.y * 10.0f + Vec3f.Dot(blended, Vec3f.Create(1.0f, 1.0f, 0.0f));
}
)RS";

    auto bundle = compileSource("p0_dual_math.rscript", kMathScript);
    runtime::EngineRuntime engine(bundle.program);

    const auto r1 = engine.invoke("P0.DualMath::LargeWorldFixed64Raw", {});
    require(r1.succeeded, "LargeWorldFixed64Raw failed: " + r1.error.message);
    std::int64_t f64Check = 0;
    require(r1.value.tryGetLong(f64Check), "LargeWorldFixed64Raw did not return long");
    require(f64Check == 2025022125LL,
        "Expected 2025022125, got " + std::to_string(f64Check));

    const auto r2 = engine.invoke("P0.DualMath::FloatVectorAndQuatPipeline", {});
    require(r2.succeeded, "FloatVectorAndQuatPipeline failed: " + r2.error.message);
    float fOut = 0.0f;
    require(r2.value.tryGetFloat(fOut), "FloatVectorAndQuatPipeline did not return float");
    require(approxEqual(fOut, 16.0, 1e-3),
        "Expected ~16.0f, got " + std::to_string(fOut));

    diagnostics::DiagnosticBag aotDiags;
    aot::GenerationOptions aotOpts;
    aotOpts.programName = "P0DualMathProgram";
    const auto gen = aot::CppGenerator{}.generate(bundle.mirModules, aotDiags, aotOpts);
    require(!aotDiags.hasErrors(), "Typed C++17 AOT generation failed for P0.DualMath");
    require(gen.source.find("runtime::Fixed64Value") != std::string::npos,
        "AOT did not emit unboxed runtime::Fixed64Value");
    require(gen.source.find("runtime::Vec2fValue") != std::string::npos,
        "AOT did not emit unboxed runtime::Vec2fValue");
    require(gen.source.find("runtime::Vec3fValue") != std::string::npos,
        "AOT did not emit unboxed runtime::Vec3fValue");
    require(gen.source.find("runtime::QuatfValue") != std::string::npos,
        "AOT did not emit unboxed runtime::QuatfValue");
}

// ----------------------------------------------------------------------------
// 3. P0-2 Packed Math Vector Arrays & Zero-Copy Bulk Read/Write APIs
// ----------------------------------------------------------------------------
void testPackedMathVectorArraysAndBulkIo() {
    std::cout << "[3/5] Testing ManagedHeap contiguous packed math arrays & zero-copy bulk I/O...\n";
    runtime::ManagedHeap heap;
    runtime::RuntimeError err;

    const auto emptyByte = heap.allocateTypedArray(
        0, semantic::PrimitiveType::Byte, 0, 0, runtime::Value{runtime::ByteValue{0}}, &err);
    require(emptyByte.has_value(), "emptyByte failed");
    const std::size_t hdrBytes = *heap.objectSizeBytes(*emptyByte);

    constexpr std::size_t kCount = 10000;

    const auto vec3fArrTid = semantic::stableTypeId(
        semantic::arrayTypeName(semantic::PrimitiveType::Struct, "RealScript.Math::Vec3f"));
    const auto v3fRef = heap.allocateTypedArray(
        vec3fArrTid,
        semantic::PrimitiveType::Struct,
        runtime::vec3fTypeId(),
        kCount,
        runtime::Value{runtime::Vec3fValue{0.0f, 0.0f, 0.0f}},
        &err);
    require(v3fRef.has_value(), "allocate Vec3f[10000] failed: " + err.message);
    require(heap.isPackedArray(*v3fRef), "Vec3f[] must use contiguous PackedStruct storage");
    require(heap.packedElementStrideBytes(*v3fRef) == 12u, "Vec3f[] stride must be 12 bytes");
    require(*heap.objectSizeBytes(*v3fRef) == hdrBytes + kCount * 12u,
        "Vec3f[10000] objectSizeBytes mismatch");

    std::vector<runtime::Vec3fValue> srcV3(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        srcV3[i] = runtime::Vec3fValue{
            static_cast<float>(i) * 0.5f,
            static_cast<float>(i) * -1.25f,
            static_cast<float>(i) + 3.0f};
    }
    require(heap.writePackedVec3fArray(*v3fRef, srcV3.data(), srcV3.size(), &err),
        "writePackedVec3fArray failed");
    std::vector<runtime::Vec3fValue> dstV3;
    require(heap.readPackedVec3fArray(*v3fRef, dstV3, &err) && dstV3 == srcV3,
        "readPackedVec3fArray round-trip mismatch");

    const auto quatfArrTid = semantic::stableTypeId(
        semantic::arrayTypeName(semantic::PrimitiveType::Struct, "RealScript.Math::Quatf"));
    const auto qfRef = heap.allocateTypedArray(
        quatfArrTid,
        semantic::PrimitiveType::Struct,
        runtime::quatfTypeId(),
        2048,
        runtime::Value{runtime::QuatfValue{0.0f, 0.0f, 0.0f, 1.0f}},
        &err);
    require(qfRef.has_value() && heap.isPackedArray(*qfRef), "Quatf[] must be packed");
    require(heap.packedElementStrideBytes(*qfRef) == 16u, "Quatf[] stride must be 16 bytes");
    std::vector<runtime::QuatfValue> srcQf(2048);
    for (std::size_t i = 0; i < srcQf.size(); ++i) {
        srcQf[i] = runtime::QuatfValue{0.1111111f, 0.2222222f, 0.3333333f, 0.70710677f};
    }
    require(heap.writePackedQuatfArray(*qfRef, srcQf.data(), srcQf.size(), &err),
        "writePackedQuatfArray failed");
    std::vector<runtime::QuatfValue> dstQf;
    require(heap.readPackedQuatfArray(*qfRef, dstQf, &err) && dstQf == srcQf,
        "readPackedQuatfArray round-trip mismatch");

    const auto f64ArrTid = semantic::stableTypeId(
        semantic::arrayTypeName(semantic::PrimitiveType::Struct, "RealScript.Math::Fixed64"));
    const auto f64Ref = heap.allocateTypedArray(
        f64ArrTid,
        semantic::PrimitiveType::Struct,
        runtime::fixed64TypeId(),
        4096,
        runtime::Value{runtime::Fixed64Value{0}},
        &err);
    require(f64Ref.has_value() && heap.isPackedArray(*f64Ref), "Fixed64[] must be packed");
    require(heap.packedElementStrideBytes(*f64Ref) == 8u, "Fixed64[] stride must be 8 bytes");
    std::vector<runtime::Fixed64Value> srcF64(4096);
    for (std::size_t i = 0; i < srcF64.size(); ++i) {
        srcF64[i] = runtime::Fixed64Value{(static_cast<std::int64_t>(i) << 32) | 0x80000000LL};
    }
    require(heap.writePackedFixed64Array(*f64Ref, srcF64.data(), srcF64.size(), &err),
        "writePackedFixed64Array failed");
    std::vector<runtime::Fixed64Value> dstF64;
    require(heap.readPackedFixed64Array(*f64Ref, dstF64, &err) && dstF64 == srcF64,
        "readPackedFixed64Array mismatch");
}

// ----------------------------------------------------------------------------
// 4. P0-2 User-Defined Blittable Struct Arrays, Nested Structs, GC O(1) & Snapshot
// ----------------------------------------------------------------------------
void testBlittableStructArraysGcAndSnapshot() {
    std::cout << "[4/5] Testing user-defined blittable struct[], nested struct[], O(1) GC & snapshot...\n";
    constexpr const char* kStructArrayScript = R"RS(
module P0.PackedStructs;
import RealScript.Math;

public struct Pos3 {
    public int x;
    public int y;
    public int z;
}

public struct Bullet {
    public Vec2f pos;
    public Vec2f vel;
    public int team;
    public float ttl;
}

public struct TransformCell {
    public Pos3 grid;
    public Vec2f sub;
}

public struct NamedEntity {
    public int id;
    public string tag;
}

public Pos3[] MakePos3Buffer(int n) {
    Pos3[] arr = new Pos3[n];
    for (int i = 0; i < n; i = i + 1) {
        Pos3 p = new Pos3();
        p.x = i;
        p.y = i * 2;
        p.z = i * 3;
        arr[i] = p;
    }
    return arr;
}

public Bullet[] StepBullets(int n) {
    Bullet[] arr = new Bullet[n];
    for (int i = 0; i < n; i = i + 1) {
        Bullet b = new Bullet();
        b.pos = Vec2f.Create((float)i, 1.0f);
        b.vel = Vec2f.Create(2.0f, -0.5f);
        b.team = i % 4;
        b.ttl = 10.0f;
        arr[i] = b;
    }
    for (int i = 0; i < n; i = i + 1) {
        Bullet b = arr[i];
        b.pos = Vec2f.Add(b.pos, b.vel);
        b.ttl = b.ttl - 0.25f;
        arr[i] = b;
    }
    return arr;
}

public TransformCell[] MakeNestedBuffer(int n) {
    TransformCell[] arr = new TransformCell[n];
    for (int i = 0; i < n; i = i + 1) {
        Pos3 g = new Pos3();
        g.x = i + 10;
        g.y = i + 20;
        g.z = i + 30;
        TransformCell t = new TransformCell();
        t.grid = g;
        t.sub = Vec2f.Create(0.25f, 0.75f);
        arr[i] = t;
    }
    return arr;
}

public int SumNested(TransformCell[] arr) {
    int acc = 0;
    for (int i = 0; i < arr.Length; i = i + 1) {
        TransformCell t = arr[i];
        acc = acc + t.grid.x + t.grid.z + (int)(t.sub.x + t.sub.y);
    }
    return acc;
}

public NamedEntity[] MakeNonBlittable(int n) {
    NamedEntity[] arr = new NamedEntity[n];
    for (int i = 0; i < n; i = i + 1) {
        NamedEntity e = new NamedEntity();
        e.id = i;
        e.tag = "unit";
        arr[i] = e;
    }
    return arr;
}
)RS";

    auto bundle = compileSource("p0_packed_structs.rscript", kStructArrayScript);
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::EngineRuntime engine(bundle.program);
    engine.setHeap(heap);

    runtime::ExecutionOptions execOpts;
    execOpts.limits.instructionBudget = 5'000'000;

    const auto rPos = engine.invoke("P0.PackedStructs::MakePos3Buffer", {std::int64_t{10000}}, execOpts);
    require(rPos.succeeded, "MakePos3Buffer failed: " + rPos.error.message);
    const auto posRef = std::get<runtime::ObjectRef>(rPos.value);
    const auto posRoot = heap->retain(rPos.value);
    require(heap->isPackedArray(posRef), "Pos3[] must be stored in PackedStruct");
    require(heap->packedElementStrideBytes(posRef) == 12u, "Pos3[] stride must be 12 bytes");
    require(*heap->objectSizeBytes(posRef) < 121000u, "Pos3[10000] must fit in ~120 KB");

    const auto rBul = engine.invoke("P0.PackedStructs::StepBullets", {std::int64_t{10000}}, execOpts);
    require(rBul.succeeded, "StepBullets failed: " + rBul.error.message);
    const auto bulRef = std::get<runtime::ObjectRef>(rBul.value);
    const auto bulRoot = heap->retain(rBul.value);
    require(heap->isPackedArray(bulRef), "Bullet[] must be stored in PackedStruct");
    require(heap->packedElementStrideBytes(bulRef) == 24u, "Bullet[] stride must be 24 bytes");
    require(*heap->objectSizeBytes(bulRef) < 241000u, "Bullet[10000] must fit in ~240 KB");

    const auto rNest = engine.invoke("P0.PackedStructs::MakeNestedBuffer", {std::int64_t{1024}}, execOpts);
    require(rNest.succeeded, "MakeNestedBuffer failed: " + rNest.error.message);
    const auto nestRef = std::get<runtime::ObjectRef>(rNest.value);
    const auto nestRoot = heap->retain(rNest.value);
    require(heap->isPackedArray(nestRef), "Nested TransformCell[] must be stored in PackedStruct");
    require(heap->packedElementStrideBytes(nestRef) == 20u, "TransformCell[] stride must be 20 bytes");

    const auto rSum = engine.invoke("P0.PackedStructs::SumNested", {rNest.value}, execOpts);
    require(rSum.succeeded, "SumNested failed: " + rSum.error.message);
    require(std::get<std::int64_t>(rSum.value) == 1089536LL,
        "SumNested mismatch: got " + std::to_string(std::get<std::int64_t>(rSum.value)));

    const auto rNamed = engine.invoke("P0.PackedStructs::MakeNonBlittable", {std::int64_t{16}}, execOpts);
    require(rNamed.succeeded, "MakeNonBlittable failed: " + rNamed.error.message);
    const auto namedRef = std::get<runtime::ObjectRef>(rNamed.value);
    require(!heap->isPackedArray(namedRef), "Struct containing string reference must NOT use PackedStruct");

    runtime::ShadowStack emptyStack;
    const std::size_t markBefore = heap->statistics().markWork;
    heap->collect(emptyStack);
    const std::size_t markDelta = heap->statistics().markWork - markBefore;
    require(markDelta <= 8u,
        "GC markWork should skip 21,024 packed struct elements in O(1), got markDelta=" +
            std::to_string(markDelta));
    require(!heap->isAlive(namedRef), "Unrooted NamedEntity[] should be collected");
    require(heap->isAlive(posRef) && heap->isAlive(bulRef) && heap->isAlive(nestRef),
        "Rooted packed struct arrays must survive GC");

    const auto snap = heap->snapshot(&emptyStack);
    runtime::RuntimeError restoreErr;
    // Mutate Pos3[777] via 64-bit int injection (demoting Pos3[]), verify demotion, then rollback via restore(snap)!
    const auto beforePos = heap->arrayGet(posRef, 777);
    require(beforePos.has_value(), "Pos3[777] missing before snapshot");
    const auto structTid = std::get<runtime::StructValue>(*beforePos).typeId;
    const std::array<runtime::Value, 3> wideFields{
        runtime::Value{5000000000LL}, runtime::Value{2LL}, runtime::Value{3LL}};
    const runtime::Value wideStruct{
        runtime::makeStructValueFromFields(
            structTid, runtime::ValueSpan(wideFields.data(), wideFields.size()))};
    require(heap->arraySet(posRef, 5, wideStruct, &restoreErr),
        "Writing 64-bit int into Pos3[] must seamlessly demote without error");
    require(!heap->isPackedArray(posRef),
        "Pos3[] should demote to HeapValues after 64-bit int injection");
    const auto wideBack = heap->arrayGet(posRef, 5);
    require(wideBack.has_value() &&
            std::get<std::int64_t>(*runtime::structValueFieldAt(
                std::get<runtime::StructValue>(*wideBack), 0)) == 5000000000LL,
        "64-bit int value after demotion mismatch");

    require(heap->restore(snap, &restoreErr), "Heap restore failed: " + restoreErr.message);
    require(heap->isPackedArray(posRef) &&
            heap->packedElementStrideBytes(posRef) == 12u,
        "Restored Pos3[] must return to PackedStruct with 12B stride");
    const auto samplePos = heap->arrayGet(posRef, 777);
    require(samplePos.has_value(), "Restored Pos3[777] missing");
    const auto* sv777 = std::get_if<runtime::StructValue>(&*samplePos);
    require(sv777 &&
            std::get<std::int64_t>(*runtime::structValueFieldAt(*sv777, 0)) == 777 &&
            std::get<std::int64_t>(*runtime::structValueFieldAt(*sv777, 1)) == 1554 &&
            std::get<std::int64_t>(*runtime::structValueFieldAt(*sv777, 2)) == 2331,
        "Restored Pos3[777] field values mismatch");

    (void)posRoot;
    (void)bulRoot;
    (void)nestRoot;
}

// ----------------------------------------------------------------------------
// 5. P0-2 Stateful Hot-Reload Migration of Live PackedStruct Arrays
// ----------------------------------------------------------------------------
void testStatefulHotReloadPackedStructMigration() {
    std::cout << "[5/5] Testing stateful hot-reload layout migration on live PackedStruct[]...\n";
    constexpr const char* kV1 = R"RS(
module P0.LiveReload;

public struct Cell {
    public int x;
    public int y;
}

public class WorldState {
    public static Cell[] cells = new Cell[4];
}

public void Init() {
    for (int i = 0; i < 4; i = i + 1) {
        Cell c = new Cell();
        c.x = (i + 1) * 10;
        c.y = (i + 1) * 100;
        WorldState.cells[i] = c;
    }
}

public Cell[] GetCells() {
    return WorldState.cells;
}
)RS";

    constexpr const char* kV2Blittable = R"RS(
module P0.LiveReload;

public struct Cell {
    public int x;
    public int y;
    public int z;
}

public class WorldState {
    public static Cell[] cells = new Cell[4];
}

public int SumAll() {
    int sum = 0;
    for (int i = 0; i < WorldState.cells.Length; i = i + 1) {
        Cell c = WorldState.cells[i];
        sum = sum + c.x + c.y + c.z;
    }
    return sum;
}

public Cell[] GetCells() {
    return WorldState.cells;
}
)RS";

    constexpr const char* kV3WithString = R"RS(
module P0.LiveReload;

public struct Cell {
    public int x;
    public int y;
    public int z;
    public string label;
}

public class WorldState {
    public static Cell[] cells = new Cell[4];
}

public int SumWithLabels() {
    int sum = 0;
    for (int i = 0; i < WorldState.cells.Length; i = i + 1) {
        Cell c = WorldState.cells[i];
        c.label = "ok";
        WorldState.cells[i] = c;
        sum = sum + c.x + c.y;
    }
    return sum;
}

public Cell[] GetCells() {
    return WorldState.cells;
}
)RS";

    auto v1 = compileSource("reload_v1.rscript", kV1);
    auto heap = std::make_shared<runtime::ManagedHeap>();
    runtime::EngineRuntime engine(v1.program);
    engine.setHeap(heap);

    require(engine.invoke("P0.LiveReload::Init", {}).succeeded, "V1 Init failed");
    const auto cellsV1 = engine.invoke("P0.LiveReload::GetCells", {});
    require(cellsV1.succeeded, "V1 GetCells failed");
    const auto arrRef = std::get<runtime::ObjectRef>(cellsV1.value);
    require(heap->isPackedArray(arrRef) && heap->packedElementStrideBytes(arrRef) == 8u,
        "V1 Cell[] must be packed at 8 bytes/element");

    auto v2 = compileSource("reload_v2.rscript", kV2Blittable);
    hot_reload::ReloadOptions reloadOpts;
    reloadOpts.allowStructuralChanges = true;
    const auto planV2 = hot_reload::apply(engine, std::move(v2.bcModules), reloadOpts);
    require(planV2.compatible, "HotReload V1->V2 failed");
    require(heap->isPackedArray(arrRef) && heap->packedElementStrideBytes(arrRef) == 12u,
        "V2 Cell[] must re-pack in-place to 12 bytes/element");

    const auto sumV2 = engine.invoke("P0.LiveReload::SumAll", {});
    require(sumV2.succeeded && std::get<std::int64_t>(sumV2.value) == 1100LL,
        "V2 SumAll expected 1100, got " + std::to_string(std::get<std::int64_t>(sumV2.value)));

    auto v3 = compileSource("reload_v3.rscript", kV3WithString);
    const auto planV3 = hot_reload::apply(engine, std::move(v3.bcModules), reloadOpts);
    require(planV3.compatible, "HotReload V2->V3 failed");
    require(!heap->isPackedArray(arrRef),
        "V3 Cell[] with string field must transition out of PackedStruct");

    const auto sumV3 = engine.invoke("P0.LiveReload::SumWithLabels", {});
    require(sumV3.succeeded && std::get<std::int64_t>(sumV3.value) == 1100LL,
        "V3 SumWithLabels expected 1100");
}

// ----------------------------------------------------------------------------
// 6. Regression Tests for Review Fixes (GC Pool Tags, Standalone AOT, Nested
//    Inline Hot-Reload, Atomic In-Place Write, Stencil JIT, Sinf, RsBindParser)
// ----------------------------------------------------------------------------
bool dummyAotEntry(aot::ExecutionContext&, const runtime::Value*, std::size_t, runtime::Value& out) {
    out = std::int64_t{42};
    return true;
}

void testReviewRegressionFixes() {
    std::cout << "[6/6] Testing review regression fixes (AOT Program, Nested Hot-Reload, Atomic Write, JIT, Sinf, RsBind)...\n";

    // (a) Inline dual-track math values must NOT be marked traceable
    require(!runtime::Value{runtime::Fixed64Value{123}}.isTraceable(), "Fixed64 must not be traceable");
    require(!runtime::Value{runtime::Vec2fValue{1.0f, 2.0f}}.isTraceable(), "Vec2f must not be traceable");
    require(!runtime::Value{runtime::Vec3fValue{1.0f, 2.0f, 3.0f}}.isTraceable(), "Vec3f must not be traceable");
    require(!runtime::Value{runtime::QuatfValue{0.0f, 0.0f, 0.0f, 1.0f}}.isTraceable(), "QuatfInline must not be traceable");
    require(runtime::Value{runtime::QuatfValue{0.1f, 0.2f, 0.3f, 0.70710677f}}.isTraceable(), "QuatfArena must be traceable");

    // (b) Standalone aot::Program(descriptor) must register BlittableStruct schemas even before invoke
    {
        constexpr semantic::SymbolId kStandaloneStructTid = 0x778899AABBCCDDEELL;
        require(!runtime::isBlittableStructType(kStandaloneStructTid),
            "Precondition: kStandaloneStructTid should not be registered yet");
        static const char* const kModules[] = {"StandaloneAotMod"};
        static const aot::FieldDescriptor kFields[] = {
            {"x", semantic::PrimitiveType::Int, 0, 0, false},
            {"y", semantic::PrimitiveType::Float, 0, 1, false},
        };
        static const aot::TypeDescriptor kTypes[] = {
            {
                kStandaloneStructTid,
                semantic::TypeKind::Struct,
                0,
                "StandaloneAotMod",
                "StandalonePair",
                kFields,
                2,
                nullptr,
                0,
                nullptr,
                0,
                false,
                nullptr,
                0,
                false,
            },
        };
        static const aot::FunctionDescriptor kFuncs[] = {
            {
                0x1122334455667788ULL,
                "StandaloneAotMod::Entry",
                semantic::PrimitiveType::Int,
                0,
                nullptr,
                nullptr,
                0,
                &dummyAotEntry,
                nullptr,
            },
        };
        static const aot::ProgramDescriptor kDesc{
            aot::RuntimeAbiMajor,
            aot::RuntimeAbiMinor,
            "StandaloneAotProgram",
            12345ULL,
            kModules,
            1,
            kTypes,
            1,
            kFuncs,
            1,
            nullptr,
            0,
        };
        aot::Program standaloneProg(kDesc);
        require(runtime::isBlittableStructType(kStandaloneStructTid),
            "Standalone aot::Program constructor must register blittable struct types");
        const auto defVal = runtime::makeDefaultBlittableStructValue(kStandaloneStructTid);
        require(defVal.has_value(), "makeDefaultBlittableStructValue must succeed for standalone AOT struct");
    }

    // (c) Hot-Reload of nested struct inside inline StructValue in PackedStruct[]
    {
        constexpr const char* kNestedV1 = R"RS(
module P0.NestedReload;
import RealScript.Math;

public struct Inner {
    public int a;
    public int b;
}

public struct Outer {
    public Inner inner;
    public Vec2f uv;
}

public class Holder {
    public static Outer[] items = new Outer[2];
}

public void Init() {
    for (int i = 0; i < 2; i = i + 1) {
        Inner inn = new Inner();
        inn.a = (i + 1) * 10;
        inn.b = (i + 1) * 20;
        Outer o = new Outer();
        o.inner = inn;
        o.uv = Vec2f.Create(1.5f, 2.5f);
        Holder.items[i] = o;
    }
}

public Outer[] GetItems() { return Holder.items; }
)RS";

        constexpr const char* kNestedV2 = R"RS(
module P0.NestedReload;
import RealScript.Math;

public struct Inner {
    public int a;
    public int b;
    public int c;
}

public struct Outer {
    public Inner inner;
    public Vec2f uv;
}

public class Holder {
    public static Outer[] items = new Outer[2];
}

public int VerifyAfterReload() {
    int sum = 0;
    for (int i = 0; i < Holder.items.Length; i = i + 1) {
        Outer o = Holder.items[i];
        sum = sum + o.inner.a + o.inner.b + o.inner.c + (int)(o.uv.x + o.uv.y);
    }
    return sum;
}
)RS";

        auto nv1 = compileSource("nested_v1.rscript", kNestedV1);
        auto nHeap = std::make_shared<runtime::ManagedHeap>();
        runtime::EngineRuntime nEngine(nv1.program);
        nEngine.setHeap(nHeap);
        require(nEngine.invoke("P0.NestedReload::Init", {}).succeeded, "Nested V1 Init failed");
        const auto itemsRes = nEngine.invoke("P0.NestedReload::GetItems", {});
        const auto itemsRef = std::get<runtime::ObjectRef>(itemsRes.value);
        require(nHeap->isPackedArray(itemsRef) && nHeap->packedElementStrideBytes(itemsRef) == 16u,
            "Nested V1 Outer[] must be packed at 16B stride");

        auto nv2 = compileSource("nested_v2.rscript", kNestedV2);
        hot_reload::ReloadOptions rOpts;
        rOpts.allowStructuralChanges = true;
        const auto plan = hot_reload::apply(nEngine, std::move(nv2.bcModules), rOpts);
        require(plan.compatible, "Nested HotReload V1->V2 failed");
        require(nHeap->isPackedArray(itemsRef) && nHeap->packedElementStrideBytes(itemsRef) == 20u,
            "Nested V2 Outer[] must stay PackedStruct and re-pack to 20B stride");
        const auto verifyRes = nEngine.invoke("P0.NestedReload::VerifyAfterReload", {});
        require(verifyRes.succeeded && std::get<std::int64_t>(verifyRes.value) == 98LL,
            "Nested V2 VerifyAfterReload expected 98, got " +
                std::to_string(std::get<std::int64_t>(verifyRes.value)));
    }

    // (d) Atomic in-place write: failed demotion (OOM budget) must NOT leave torn field 0 write
    {
        runtime::HeapConfig cfg;
        cfg.initialCollectionThresholdBytes = 1024;
        cfg.maximumHeapBytes = 4096;
        runtime::ManagedHeap cappedHeap(cfg);
        runtime::RuntimeError err;
        const auto posTid = semantic::stableTypeId("P0.PackedStructs::Pos3");
        const auto posArrTid = semantic::stableTypeId(
            semantic::arrayTypeName(semantic::PrimitiveType::Struct, "P0.PackedStructs::Pos3"));
        const std::array<runtime::Value, 3> initFields{
            runtime::Value{11LL}, runtime::Value{22LL}, runtime::Value{33LL}};
        const runtime::Value initStruct{
            runtime::makeStructValueFromFields(posTid, runtime::ValueSpan(initFields.data(), 3))};
        // 200 elements * 12B = 2400B (fits in 4096B), but demoting to 200 * 16B = 3200B + overhead exceeds 4096B if we allocate another array
        const auto arrRef = cappedHeap.allocateTypedArray(
            posArrTid, semantic::PrimitiveType::Struct, posTid, 200, initStruct, &err);
        require(arrRef.has_value() && cappedHeap.isPackedArray(*arrRef), "capped Pos3[200] alloc failed");
        // Fill remaining budget so demotion cannot allocate 3200B
        const auto filler = cappedHeap.allocateTypedArray(
            0, semantic::PrimitiveType::Byte, 0, 1200, runtime::Value{runtime::ByteValue{1}}, &err);
        require(filler.has_value(), "filler alloc failed");

        const std::array<runtime::Value, 3> tornCandidate{
            runtime::Value{999LL}, runtime::Value{5000000000LL}, runtime::Value{777LL}};
        const runtime::Value tornStruct{
            runtime::makeStructValueFromFields(posTid, runtime::ValueSpan(tornCandidate.data(), 3))};
        require(!cappedHeap.arraySet(*arrRef, 0, tornStruct, &err) &&
                err.code == runtime::ErrorCode::OutOfMemory,
            "Demotion under tight heap budget should fail with OutOfMemory");
        const auto afterFailedWrite = cappedHeap.arrayGet(*arrRef, 0);
        require(afterFailedWrite.has_value(), "arrayGet after failed write must succeed");
        const auto& sv0 = std::get<runtime::StructValue>(*afterFailedWrite);
        require(std::get<std::int64_t>(*runtime::structValueFieldAt(sv0, 0)) == 11LL &&
                std::get<std::int64_t>(*runtime::structValueFieldAt(sv0, 1)) == 22LL &&
                std::get<std::int64_t>(*runtime::structValueFieldAt(sv0, 2)) == 33LL,
            "Failed demotion caused a torn write in PackedStruct element 0!");
    }

    // (e) Stencil JIT NewArrayFast on user-defined blittable struct + Math.Sinf on huge/Inf/NaN
    {
        constexpr const char* kJitAndSinScript = R"RS(
module P0.JitAndSin;
import RealScript.Math;

public struct GridPt {
    public int x;
    public int y;
}

public GridPt[] AllocGrid(int n) {
    return new GridPt[n];
}

public float TestEdgeSinf() {
    float a = Math.Sinf(10000000.0f);
    float b = Math.Sinf(1000000000.0f * 1000000000.0f);
    float inf = 1.0f / 0.0f;
    float c = Math.Sinf(inf);
    return a * 0.0f + b + c;
}
)RS";
        auto jb = compileSource("jit_and_sin.rscript", kJitAndSinScript);
        auto jHeap = std::make_shared<runtime::ManagedHeap>();
        const auto jitCompiled = jit::StencilJit{}.compile(jb.program);
        require(jitCompiled.succeeded(), "StencilJit::compile failed: " + jitCompiled.error);
        jitCompiled.module->setHeap(jHeap);
        require(jitCompiled.module->functionTier("P0.JitAndSin::AllocGrid") !=
                jit::StencilTier::InterpreterFallback,
            "AllocGrid should be eligible for StencilJit");
        for (int iter = 0; iter < 3; ++iter) {
            const auto gr = jitCompiled.module->invoke("P0.JitAndSin::AllocGrid", {std::int64_t{8}});
            require(gr.succeeded, "Stencil JIT AllocGrid failed: " + gr.error.message);
            const auto gRef = std::get<runtime::ObjectRef>(gr.value);
            require(jHeap->isPackedArray(gRef) && jHeap->packedElementStrideBytes(gRef) == 8u,
                "Stencil JIT AllocGrid must produce PackedStruct array with 8B stride");
        }
        runtime::ExecutionOptions jOpts;
        jOpts.limits.instructionBudget = 5000;
        const auto sinRes = jitCompiled.module->invoke("P0.JitAndSin::TestEdgeSinf", {}, jOpts);
        require(sinRes.succeeded, "TestEdgeSinf exceeded instruction budget or failed: " + sinRes.error.message);
    }

    // (f) Fixed64::toInt() floor semantics & RsBindParser Vec2f/Vec3f/Quatf/Fixed64 mapping
    {
        const auto negHalf = game::Fixed64::fromDouble(-0.5);
        require(negHalf.toInt() == -1 && negHalf.toIntFloor() == -1,
            "Fixed64::toInt() must floor negative values (-0.5 -> -1) matching Fixed64.ToInt()");
        require(negHalf.toIntTrunc() == 0,
            "Fixed64::toIntTrunc() must truncate toward zero (-0.5 -> 0)");

        constexpr const char* kHeader = R"CPP(
namespace demo {
RS_EXPORT_ECS_COMPONENT()
struct TransformComp {
    Fixed64 worldTick;
    Vec2f uv;
    Vec3f position;
    Quatf rotation;
};
}
)CPP";
        const auto gen = game::rsbind_detail::generateFromParsedHeader(kHeader, "Demo.Bindings", {});
        require(gen.structsExported == 1, "RsBindParser failed on dual-track math ECS component");
        require(gen.scriptDeclarationStub.find("public Fixed64 worldTick;") != std::string::npos &&
                gen.scriptDeclarationStub.find("public Vec2f uv;") != std::string::npos &&
                gen.scriptDeclarationStub.find("public Vec3f position;") != std::string::npos &&
                gen.scriptDeclarationStub.find("public Quatf rotation;") != std::string::npos,
            "RsBindParser mis-mapped Fixed64/Vec2f/Vec3f/Quatf in .d.rscript stub:\n" +
                gen.scriptDeclarationStub);
    }
}

} // namespace

int main() {
    try {
        testValueAbiAndMarshal();
        testDualTrackMathExecutionAndAotCodegen();
        testPackedMathVectorArraysAndBulkIo();
        testBlittableStructArraysGcAndSnapshot();
        testStatefulHotReloadPackedStructMigration();
        testReviewRegressionFixes();
        std::cout << "All P0 dual-track math & contiguous PackedStruct tests PASSED!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FATAL: " << ex.what() << "\n";
        return 1;
    }
}
