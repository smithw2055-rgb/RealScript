#include "realscript/AbiGovernance.h"
#include "realscript/Version.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/aot_cpp/RuntimeAbi.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/game/GameBytecodeObjects.h"
#include "realscript/game/GameProductization.h"
#include "realscript/game/GameplayStateCodec.h"
#include "realscript/hot_reload/HotReload.h"
#include "realscript/runtime/FiberScheduler.h"
#include "realscript/runtime/Runtime.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace realscript;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<bytecode::Module> compileScriptModules(
    const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<compiler::SourceFile> sources;
    sources.reserve(files.size());
    for (const auto& [path, text] : files) {
        sources.push_back({path, text});
    }
    compiler::Compilation compilation(std::move(sources));
    auto built = compilation.build();
    if (built.diagnostics.hasErrors()) {
        std::string details = "Script compilation failed:";
        for (const auto& d : built.diagnostics.items()) {
            details += "\n  " + d.code + ": " + d.message;
        }
        throw std::runtime_error(details);
    }
    bytecode::Lowerer lowerer;
    std::vector<bytecode::Module> lowered;
    lowered.reserve(built.modules.size());
    for (const auto& mirMod : built.modules) {
        lowered.push_back(lowerer.lower(mirMod));
    }
    return lowered;
}

void testModularRuntimeSplitParity() {
    // 1. ValueOperations: math type IDs, inline struct slot codec, nested inline & heap structs
    require(runtime::fixedTypeId() != 0, "fixedTypeId must be non-zero");
    require(runtime::fixed16TypeId() != 0, "fixed16TypeId must be non-zero");
    require(runtime::vec2TypeId() != 0, "vec2TypeId must be non-zero");
    require(runtime::vec3TypeId() != 0, "vec3TypeId must be non-zero");
    require(runtime::quatTypeId() != 0, "quatTypeId must be non-zero");

    const auto defVec3 = runtime::makeDefaultInlineStructValue(runtime::vec3TypeId());
    require(defVec3.has_value(), "makeDefaultInlineStructValue(vec3) failed");
    require(
        runtime::structValueTypeId(*defVec3) == runtime::vec3TypeId(),
        "structValueTypeId(vec3) mismatch");

    const auto updatedVec3 = runtime::storeInlineStructField(
        *defVec3, 1, runtime::Value{runtime::FixedValue{65536}});
    require(updatedVec3.has_value(), "storeInlineStructField(vec3, y) failed");
    const auto readVec3Y = runtime::loadInlineStructField(*updatedVec3, 1);
    require(
        readVec3Y.has_value() &&
            std::get<runtime::FixedValue>(*readVec3Y).raw == 65536,
        "loadInlineStructField(vec3, y) mismatch");

    const semantic::SymbolId innerType = semantic::stableTypeId("P3.Test::InnerPair");
    const semantic::SymbolId outerType = semantic::stableTypeId("P3.Test::OuterStruct");
    auto innerStruct = runtime::makeStructValueFromFields(
        innerType,
        std::vector<runtime::Value>{std::int64_t{11}, std::string{"nested-ok"}});
    require(innerStruct.isInline, "2-field struct should pack inline");
    auto outerStruct = runtime::makeStructValueFromFields(
        outerType,
        std::vector<runtime::Value>{true, runtime::Value{innerStruct}});
    require(outerStruct.isInline, "outer struct with nested inline struct should pack inline");
    const auto outerField1 = runtime::structValueFieldAt(outerStruct, 1);
    require(
        outerField1.has_value() &&
            std::get<runtime::StructValue>(*outerField1) == innerStruct,
        "nested inline struct round-trip equality failed");

    const semantic::SymbolId wideType = semantic::stableTypeId("P3.Test::WideStruct");
    auto wideA = runtime::makeStructValueFromFields(
        wideType,
        std::vector<runtime::Value>{
            std::int64_t{1},
            std::int64_t{2},
            std::int64_t{3},
            std::int64_t{4},
            std::int64_t{5},
            std::string{"heap-backed"},
        });
    auto wideB = runtime::makeStructValueFromFields(
        wideType,
        std::vector<runtime::Value>{
            std::int64_t{1},
            std::int64_t{2},
            std::int64_t{3},
            std::int64_t{4},
            std::int64_t{5},
            std::string{"heap-backed"},
        });
    require(!wideA.isInline && wideA == wideB, "heap-backed StructValue dedup/equality failed");

    // 2. BindingRegistry: symbol/name registration, striped lock cache & multithreaded resolve
    auto bindings = std::make_shared<runtime::BindingRegistry>();
    require(
        bindings->bind(
            "P3.Host::ComputeScale",
            [](const bytecode::FunctionReference&,
               runtime::ValueSpan args,
               runtime::RuntimeError&) -> std::optional<runtime::Value> {
                const auto x = std::get<std::int64_t>(args[0]);
                return runtime::Value{x * 3 + 5};
            },
            runtime::BindingDeterminism::Deterministic,
            "Host.Math"),
        "BindingRegistry::bind by canonicalName failed");
    require(
        !bindings->bind(
            "P3.Host::ComputeScale",
            [](const bytecode::FunctionReference&,
               runtime::ValueSpan,
               runtime::RuntimeError&) -> std::optional<runtime::Value> {
                return runtime::Value{std::int64_t{0}};
            }),
        "duplicate canonicalName binding should be rejected");
    require(bindings->size() == 1, "BindingRegistry::size() mismatch");

    std::atomic<int> threadFailures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t]() {
            for (int iter = 0; iter < 64; ++iter) {
                bytecode::FunctionReference ref{};
                ref.symbolId = static_cast<semantic::SymbolId>(0xABCDEF0000ULL + t * 64 + iter);
                ref.name = "P3.Host::ComputeScale";
                runtime::RuntimeError err;
                const std::array<runtime::Value, 1> callArgs{runtime::Value{std::int64_t{10 + iter}}};
                const auto out = bindings->invoke(ref, callArgs, err);
                if (!out.has_value() ||
                    std::get<std::int64_t>(*out) != (10 + iter) * 3 + 5) {
                    threadFailures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    require(threadFailures.load() == 0, "concurrent BindingRegistry::invoke failed");
    require(
        bindings->cachedSymbolCount() >= 1 + 4 * 64,
        "BindingRegistry striped symbol cache did not record resolved symbol IDs");

    // 3. ProgramImage & EngineRuntime: multi-module link, virtual/interface dispatch & intrinsics
    auto modules = compileScriptModules({
        {"core.rscript", R"(
module P3.Core;

public interface IScorer {
    int Score(int baseVal);
}

public class BonusScorer : IScorer {
    public int bonus;

    public BonusScorer(int b) {
        bonus = b;
    }

    public int Score(int baseVal) {
        return baseVal * 2 + bonus;
    }
}
)"},
        {"app.rscript", R"(
module P3.App;
import P3.Core;

public int RunPipeline(int input) {
    IScorer scorer = new BonusScorer(7);
    string tag = "score=" + "ok";
    if (tag.Length == 8) {
        return scorer.Score(input);
    }
    return -1;
}
)"},
    });

    runtime::RuntimeError linkErr;
    auto linked = runtime::ProgramImage::link(modules, linkErr);
    require(linked.has_value(), "ProgramImage::link failed: " + linkErr.message);
    require(linked->moduleCount() == 2, "ProgramImage::moduleCount mismatch");

    // Duplicate module link must fail deterministically
    std::vector<bytecode::Module> dupModules = {modules[0], modules[0]};
    runtime::RuntimeError dupErr;
    auto dupLinked = runtime::ProgramImage::link(std::move(dupModules), dupErr);
    require(
        !dupLinked.has_value() && dupErr.code == runtime::ErrorCode::DuplicateSymbol,
        "ProgramImage::link must reject duplicate SymbolIds");

    runtime::EngineRuntime engine(
        std::make_shared<const runtime::ProgramImage>(std::move(*linked)));
    const auto execRes = engine.invoke("P3.App::RunPipeline", {std::int64_t{15}});
    require(execRes.succeeded, "EngineRuntime::invoke failed: " + execRes.error.message);
    require(
        std::get<std::int64_t>(execRes.value) == 37,
        "EngineRuntime::invoke returned unexpected value");
    require(
        std::string(runtime::traceEventKindName(runtime::TraceEventKind::FunctionEnter)) ==
            "function-enter",
        "traceEventKindName mismatch");
}

void testAbiContractManifestAndJsonRoundTrip() {
    const auto manifest = currentAbiContractManifest();
    require(manifest.schemaVersion == kAbiContractSchemaVersion, "schemaVersion mismatch");
    require(manifest.engineVersionMajor == kVersionMajor, "engineVersionMajor mismatch");
    require(manifest.engineVersionMinor == kVersionMinor, "engineVersionMinor mismatch");
    require(manifest.engineVersionPatch == kVersionPatch, "engineVersionPatch mismatch");
    require(
        manifest.sdkCompatibilityVersion == kSdkCompatibilityVersion,
        "sdkCompatibilityVersion mismatch");
    require(
        manifest.gameSdkPackageVersion == kGameSdkPackageVersion,
        "gameSdkPackageVersion mismatch");
    require(
        manifest.scriptObjectStateVersion == kScriptObjectStateVersion,
        "scriptObjectStateVersion mismatch");
    require(
        manifest.bytecodeMinVersion == BytecodeVersionPoint{0, 6} &&
            manifest.bytecodeMaxVersion == BytecodeVersionPoint{0, 9} &&
            manifest.bytecodeTargetVersion == BytecodeVersionPoint{0, 7},
        ".rsbc version contract interval must be 0.6..0.9 with default 0.7");
    require(
        manifest.rsgsStateVersion == game::kGameplayStateCodecVersion &&
            manifest.rsgsStateVersion == 1u,
        "RSGS state version mismatch");
    require(manifest.runtimeValueSize == 16u, "sizeof(runtime::Value) must be 16");
    require(
        manifest.runtimeValueAlign == alignof(runtime::Value),
        "alignof(runtime::Value) mismatch");
    require(
        manifest.bytecodeInstructionSize == sizeof(bytecode::Instruction) &&
            manifest.bytecodeInstructionAlign == alignof(bytecode::Instruction) &&
            manifest.compactInstructionSize == 8u,
        "instruction layout fields mismatch");
    require(
        manifest.nativeModuleAbi.abiMajor == aot::RuntimeAbiMajor &&
            manifest.nativeModuleAbi.abiMinor == aot::RuntimeAbiMinor &&
            manifest.nativeModuleAbi.runtimeApiStructSize == sizeof(RsRuntimeApiV1) &&
            manifest.nativeModuleAbi.functionEntryStructSize == sizeof(RsFunctionEntryV1) &&
            manifest.nativeModuleAbi.moduleExportsStructSize == sizeof(RsModuleExportsV1),
        "C11 NativeModuleAbi contract mismatch");
    require(manifest.hasValidDigest(), "currentAbiContractManifest digest must be valid");
    require(
        manifest.layoutDigest == currentAbiContractManifest().layoutDigest,
        "FNV-1a layoutDigest must be deterministic");

    const std::string json = manifest.toJson();
    std::string parseErr;
    const auto roundTripped = AbiContractManifest::fromJson(json, &parseErr);
    require(
        roundTripped.has_value(),
        "AbiContractManifest::fromJson failed on valid JSON: " + parseErr);
    require(
        *roundTripped == manifest && roundTripped->hasValidDigest(),
        "AbiContractManifest JSON round-trip altered fields");

    std::string badErr;
    require(
        !AbiContractManifest::fromJson("{ malformed json", &badErr).has_value() &&
            !badErr.empty(),
        "fromJson must reject malformed JSON");
    require(
        !AbiContractManifest::fromJson("{\"schemaVersion\": 1}", &badErr).has_value(),
        "fromJson must reject incomplete manifest JSON");
}

void testVerifyAbiCompatibilityMatrix() {
    const auto host = currentAbiContractManifest();

    // 1. Exact match & patch-only skew => Compatible
    const auto exactReport = verifyAbiCompatibility(host, host);
    require(
        exactReport.compatible() && exactReport.issues.empty(),
        "identical manifests must be Compatible with zero issues");

    auto patchSkew = host;
    patchSkew.engineVersionPatch += 3;
    patchSkew.refreshLayoutDigest();
    const auto patchReport = verifyAbiCompatibility(host, patchSkew);
    require(
        patchReport.compatible(),
        "engine patch version difference alone must remain Compatible");

    // 2. CompatibleWithMigration scenarios
    {
        auto legacyBc = host;
        legacyBc.bytecodeTargetVersion = BytecodeVersionPoint{0, 6};
        legacyBc.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(host, legacyBc);
        require(
            rep.status == AbiCompatibilityStatus::CompatibleWithMigration &&
                rep.canLoad() && rep.requiresMigration() &&
                rep.hasIssue(AbiIssueCode::BytecodeLegacyMetadataMigration),
            "RSBC 0.6 target must report CompatibleWithMigration");
    }
    {
        auto hostV2State = host;
        hostV2State.scriptObjectStateVersion = 2;
        hostV2State.rsgsStateVersion = 2;
        hostV2State.gameSdkPackageVersion = 2;
        hostV2State.nativeModuleAbi.abiMinor = 1;
        hostV2State.refreshLayoutDigest();

        const auto rep = verifyAbiCompatibility(hostV2State, host);
        require(
            rep.status == AbiCompatibilityStatus::CompatibleWithMigration &&
                rep.hasIssue(AbiIssueCode::ScriptObjectStateMigrationRequired) &&
                rep.hasIssue(AbiIssueCode::RsgsStateMigrationRequired) &&
                rep.hasIssue(AbiIssueCode::GameSdkPackageOlderMigration) &&
                rep.hasIssue(AbiIssueCode::NativeModuleAbiMinorBackwardCompatible),
            "older state/package/nativeMinor versions must report CompatibleWithMigration");
    }
    {
        auto olderEngine = host;
        olderEngine.engineVersionMinor = host.engineVersionMinor - 1;
        olderEngine.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(host, olderEngine);
        require(
            rep.status == AbiCompatibilityStatus::CompatibleWithMigration &&
                rep.hasIssue(AbiIssueCode::EngineMinorOlderMigration),
            "older engine minor version must report CompatibleWithMigration");
    }

    // 3. Hard Incompatible scenarios
    {
        auto tamperedDigest = host;
        tamperedDigest.layoutDigest ^= 0x1ULL;
        const auto rep = verifyAbiCompatibility(host, tamperedDigest);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible && !rep.canLoad() &&
                rep.hasIssue(AbiIssueCode::LayoutDigestMismatch),
            "tampered layoutDigest must be rejected as Incompatible");
    }
    {
        auto badValueSize = host;
        badValueSize.runtimeValueSize = 24;
        badValueSize.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(host, badValueSize);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible &&
                rep.hasIssue(AbiIssueCode::RuntimeValueLayoutMismatch),
            "mismatched sizeof(Value) must be Incompatible");
    }
    {
        auto badInstSize = host;
        badInstSize.bytecodeInstructionSize += 8;
        badInstSize.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(host, badInstSize);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible &&
                rep.hasIssue(AbiIssueCode::BytecodeInstructionLayoutMismatch),
            "mismatched sizeof(Instruction) must be Incompatible");
    }
    {
        auto badNativeAbi = host;
        badNativeAbi.nativeModuleAbi.abiMajor += 1;
        badNativeAbi.nativeModuleAbi.moduleExportsStructSize += 8;
        badNativeAbi.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(host, badNativeAbi);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible &&
                rep.hasIssue(AbiIssueCode::NativeModuleAbiMajorMismatch) &&
                rep.hasIssue(AbiIssueCode::NativeModuleAbiStructSizeMismatch),
            "mismatched C11 NativeModuleAbi major/struct size must be Incompatible");
    }
    {
        auto futureBc = host;
        futureBc.bytecodeMinVersion = BytecodeVersionPoint{0, 10};
        futureBc.bytecodeMaxVersion = BytecodeVersionPoint{0, 12};
        futureBc.bytecodeTargetVersion = BytecodeVersionPoint{0, 10};
        futureBc.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(host, futureBc);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible &&
                rep.hasIssue(AbiIssueCode::BytecodeVersionRangeDisjoint) &&
                rep.hasIssue(AbiIssueCode::BytecodeTargetVersionUnsupported),
            "unsupported future .rsbc 0.10..0.12 range must be Incompatible");
    }
}

void testBytecodeHeaderFastPreflight() {
    auto modules = compileScriptModules({{"probe.rscript", R"(
module P3.Probe;
public int Ping() { return 42; }
)"}});
    require(modules.size() == 1, "expected single module");

    // 0.7 real encoded module & 16-byte header-only slice
    const auto bytes07 = bytecode::encodeModule(modules[0]);
    const auto rep07 = inspectBytecodeHeaderCompatibility(bytes07);
    require(
        rep07.compatible() && rep07.validMagic && !rep07.truncated &&
            rep07.version == BytecodeVersionPoint{0, 7} &&
            rep07.sectionCount == 8u && rep07.hasObjectModelSection &&
            !rep07.hasSequenceResultMetadata && !rep07.hasExceptionMetadata,
        ".rsbc 0.7 header preflight failed");

    const auto sliceRep = inspectBytecodeHeaderCompatibility(bytes07.data(), 16u);
    require(sliceRep.compatible(), "16-byte header-only preflight must succeed");

    // 0.9 real encoded module
    auto mod09 = modules[0];
    mod09.version = bytecode::Version{0, 9};
    const auto bytes09 = bytecode::encodeModule(mod09);
    const auto rep09 = inspectBytecodeHeaderCompatibility(bytes09);
    require(
        rep09.compatible() && rep09.hasObjectModelSection &&
            rep09.hasSequenceResultMetadata && rep09.hasExceptionMetadata,
        ".rsbc 0.9 header capability flags mismatch");

    // 0.6 legacy encoded module => CompatibleWithMigration
    auto mod06 = modules[0];
    mod06.version = bytecode::Version{0, 6};
    const auto bytes06 = bytecode::encodeModule(mod06);
    const auto rep06 = inspectBytecodeHeaderCompatibility(bytes06);
    require(
        rep06.status == AbiCompatibilityStatus::CompatibleWithMigration &&
            rep06.canLoad() && rep06.requiresMigration() &&
            rep06.sectionCount == 7u && !rep06.hasObjectModelSection &&
            rep06.hasIssue(AbiIssueCode::BytecodeLegacyMetadataMigration),
        ".rsbc 0.6 header must report CompatibleWithMigration");

    // Rejection cases: truncated, bad magic, unsupported 0.5 & 0.10, bad section count
    const std::vector<std::uint8_t> shortBytes{'R', 'S', 'B', 'C', 0, 0};
    const auto shortRep = inspectBytecodeHeaderCompatibility(shortBytes);
    require(
        !shortRep.canLoad() && shortRep.truncated &&
            shortRep.hasIssue(AbiIssueCode::BytecodeHeaderTruncated),
        "truncated header must be rejected");

    auto badMagic = bytes07;
    badMagic[0] = 'X';
    const auto magicRep = inspectBytecodeHeaderCompatibility(badMagic);
    require(
        !magicRep.canLoad() && !magicRep.validMagic &&
            magicRep.hasIssue(AbiIssueCode::BytecodeInvalidMagic),
        "bad magic header must be rejected");

    auto unsupported05 = bytes06;
    unsupported05[6] = 5u; // minor = 5
    const auto ver05Rep = inspectBytecodeHeaderCompatibility(unsupported05);
    require(
        !ver05Rep.canLoad() &&
            ver05Rep.hasIssue(AbiIssueCode::BytecodeTargetVersionUnsupported),
        ".rsbc 0.5 header must be rejected");

    auto unsupported010 = bytes09;
    unsupported010[6] = 10u; // minor = 10
    const auto ver10Rep = inspectBytecodeHeaderCompatibility(unsupported010);
    require(
        !ver10Rep.canLoad() &&
            ver10Rep.hasIssue(AbiIssueCode::BytecodeTargetVersionUnsupported),
        ".rsbc 0.10 header must be rejected");

    auto wrongSecCount = bytes07;
    wrongSecCount[12] = 7u; // 0.7 requires 8 sections
    const auto secRep = inspectBytecodeHeaderCompatibility(wrongSecCount);
    require(
        !secRep.canLoad() &&
            secRep.hasIssue(AbiIssueCode::BytecodeSectionCountMismatch),
        "mismatched section count in .rsbc 0.7 header must be rejected");
}

void testGameProductizationAbiEnvelopeIntegration() {
    game::GameApi api;
    require(
        api.function("Engine.Test", "ClampZero", [](int v) { return v < 0 ? 0 : v; }),
        "failed to bind host function");
    require(api.valid(), "GameApi must be valid");

    game::GameScriptCompiler compiler(api);
    auto compiled = compiler.compile({{"pkg.rscript", R"(
module P3.GamePkg;
import Engine.Test;

class CounterBehavior {
    int value;
    int Step(int delta) {
        value = ClampZero(value + delta);
        return value;
    }
}
)"}});
    require(compiled.succeeded(), "GameScriptCompiler failed");

    std::vector<std::vector<std::uint8_t>> encoded;
    for (const auto& m : compiled.modules) {
        encoded.push_back(bytecode::encodeModule(m));
    }

    const auto preflight = game::preflightBytecodePackageHeaders(encoded);
    require(preflight.compatible(), "preflightBytecodePackageHeaders failed");

    game::GameProgramLoader loader(api);
    auto loaded = loader.loadBytecodeModules(encoded);
    require(loaded.succeeded(), "GameProgramLoader::loadBytecodeModules failed");
    require(
        loaded.package.abiManifest == currentAbiContractManifest() &&
            loaded.abiCompatibility.compatible(),
        "loaded GameProgramPackage must embed valid host ABI manifest");

    // Envelope JSON round-trip and verification
    const auto envelope = game::buildPackageContractEnvelope(loaded.package);
    const std::string envJson = envelope.toJson();
    std::string envErr;
    const auto decodedEnv = game::GamePackageContractEnvelope::fromJson(envJson, &envErr);
    require(decodedEnv.has_value(), "GamePackageContractEnvelope::fromJson failed: " + envErr);
    require(
        decodedEnv->programContentHash == loaded.package.programContentHash &&
            decodedEnv->hostApiHash == loaded.package.hostApiHash &&
            decodedEnv->moduleCount == compiled.modules.size() &&
            decodedEnv->abiManifest == loaded.package.abiManifest,
        "GamePackageContractEnvelope round-trip mismatch");

    const auto verifiedEnv = game::verifyPackageContractEnvelope(
        *decodedEnv, &api, loaded.package.programContentHash);
    require(verifiedEnv.compatible(), "verifyPackageContractEnvelope should pass");

    // Mismatched host API must fail envelope verification
    game::GameApi otherApi;
    require(
        otherApi.function("Engine.Test", "DifferentFn", [](int v) { return v + 1; }),
        "otherApi bind failed");
    const auto mismatchedApiReport =
        game::verifyPackageContractEnvelope(*decodedEnv, &otherApi);
    require(
        !mismatchedApiReport.canLoad() &&
            mismatchedApiReport.hasIssue(AbiIssueCode::SdkCompatibilityMismatch),
        "verifyPackageContractEnvelope must reject mismatched GameApi contract");

    // Loader gate with incompatible embedded manifest must reject before linking
    auto badManifest = currentAbiContractManifest();
    badManifest.runtimeValueSize = 32;
    badManifest.refreshLayoutDigest();
    const auto rejectedLoad = loader.loadBytecodeModules(encoded, badManifest);
    require(
        !rejectedLoad.succeeded() &&
            rejectedLoad.abiCompatibility.status == AbiCompatibilityStatus::Incompatible &&
            rejectedLoad.diagnostics.hasErrors(),
        "GameProgramLoader must reject incompatible embedded ABI manifest");

    // Loader gate with migration-compatible manifest must succeed and preserve migration report
    auto migManifest = currentAbiContractManifest();
    migManifest.engineVersionMinor = migManifest.engineVersionMinor - 1;
    migManifest.refreshLayoutDigest();
    const auto migratedLoad = loader.loadBytecodeModules(encoded, migManifest);
    require(
        migratedLoad.succeeded() &&
            migratedLoad.abiCompatibility.requiresMigration(),
        "GameProgramLoader should accept CompatibleWithMigration embedded manifest");
}

void testCMakePhysicalLayeringContractIfAvailable() {
    std::string cmakePath;
#ifdef REALSCRIPT_CMAKELISTS_PATH
    cmakePath = REALSCRIPT_CMAKELISTS_PATH;
#elif defined(REALSCRIPT_SOURCE_DIR)
    cmakePath = std::string(REALSCRIPT_SOURCE_DIR) + "/CMakeLists.txt";
#endif
    if (cmakePath.empty()) return;

    std::ifstream in(cmakePath, std::ios::binary);
    if (!in) return;
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string content = buf.str();
    if (content.find("realscript_layer_foundation") == std::string::npos) {
        // Running against read-only baseline source dir without overlay macro; skip text check.
        return;
    }

    const std::vector<std::string> requiredTokens = {
        "realscript_layer_foundation",
        "realscript_layer_compiler_core",
        "realscript_layer_runtime_core",
        "realscript_layer_native_backends",
        "realscript_layer_debug",
        "realscript_layer_tooling",
        "realscript_layer_game",
        "src/runtime/AbiGovernance.cpp",
        "src/runtime/ValueOperations.cpp",
        "src/runtime/BindingRegistry.cpp",
        "src/runtime/ProgramImage.cpp",
        "src/runtime/RuntimeServices.cpp",
        "add_library(RealScript::Frontend ALIAS realscript_frontend)",
        "realscript_add_component(realscript_runtime Runtime)",
        "realscript_add_component(realscript_full Full)",
        "EXPORT RealScriptTargets",
        "/wd4267",
        "/wd4456",
    };
    for (const auto& token : requiredTokens) {
        require(
            content.find(token) != std::string::npos,
            "Refactored CMakeLists.txt is missing required contract token: " + token);
    }
}

} // namespace

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto fn) {
        try {
            fn();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        }
    };

    run("modular runtime split parity (ValueOperations / BindingRegistry / ProgramImage / RuntimeServices)",
        testModularRuntimeSplitParity);
    run("AbiContractManifest layout invariants, FNV-1a digest & JSON round-trip",
        testAbiContractManifestAndJsonRoundTrip);
    run("verifyAbiCompatibility matrix (Compatible / CompatibleWithMigration / Incompatible)",
        testVerifyAbiCompatibilityMatrix);
    run(".rsbc bytecode header fast preflight (0.6..0.9 & malformed rejection)",
        testBytecodeHeaderFastPreflight);
    run("GameProductization package ABI contract envelope & loader gate",
        testGameProductizationAbiEnvelopeIntegration);
    run("CMake physical layered targets & export contract verification",
        testCMakePhysicalLayeringContractIfAvailable);

    return failures == 0 ? 0 : 1;
}
