#include "realscript/AbiGovernance.h"
#include "realscript/HostAbi.h"
#include "realscript/Version.h"
#include "realscript/aot_cpp/AotRuntime.h"
#include "realscript/aot_cpp/RuntimeAbi.h"
#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "failed to open file: " + path.string());
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
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

void testCMakeSliceTopology() {
#ifndef REALSCRIPT_SOURCE_DIR
    require(false, "REALSCRIPT_SOURCE_DIR compile definition is required");
#else
    const std::filesystem::path root(REALSCRIPT_SOURCE_DIR);
    const std::string cmake = readTextFile(root / "CMakeLists.txt");

    // 1. Verify Tier-0 L0 physical slices and OBJECT layers
    const std::vector<std::string> requiredTokens = {
        "realscript_layer_foundation",
        "realscript_layer_bytecode_format",
        "realscript_layer_compiler_core",
        "realscript_layer_runtime_core",
        "realscript_layer_native_backends",
        "realscript_slice_foundation_core",
        "realscript_slice_bytecode_core",
        "realscript_slice_compiler_core",
        "realscript_slice_runtime_core",
        "realscript_slice_native_backends",
        "add_library(RealScript::Kernel ALIAS realscript_kernel)",
        "src/debug/DebugInfo.cpp",
        "src/semantic/TypeMetadata.cpp",
        "src/text/AssetGuid.cpp",
        "src/runtime/HostAbi.cpp",
        "src/runtime/AbiGovernance.cpp",
        "src/runtime/ValueOperations.cpp",
        "src/runtime/BindingRegistry.cpp",
        "src/runtime/ProgramImage.cpp",
        "src/runtime/RuntimeServices.cpp",
        // 2. Verify orthogonal Tier-1 L1 options & apps
        "REALSCRIPT_ENABLE_DEBUG_SERVICES",
        "REALSCRIPT_ENABLE_TOOLING",
        "REALSCRIPT_ENABLE_EXTENSIBILITY",
        "REALSCRIPT_ENABLE_GAME_SDK",
        "realscript_app_debug",
        "realscript_app_tooling",
        "realscript_app_extensibility",
        "realscript_app_game_sdk",
        "EXPORT RealScriptTargets",
    };
    for (const auto& token : requiredTokens) {
        require(
            cmake.find(token) != std::string::npos,
            "CMakeLists.txt missing required slice/layer contract token: " + token);
    }

    // 3. Verify DebugInfo.cpp lives in bytecode format layer, and TypeMetadata/AssetGuid live in foundation
    const auto findSection = [&](const std::string& marker) -> std::string {
        const auto start = cmake.find(marker);
        require(start != std::string::npos, "missing CMake section: " + marker);
        const auto end = cmake.find(')', start);
        require(end != std::string::npos, "unterminated CMake section: " + marker);
        return cmake.substr(start, end - start);
    };

    const std::string foundationLayer = findSection("realscript_add_layer_library(realscript_layer_foundation");
    require(
        foundationLayer.find("src/semantic/TypeMetadata.cpp") != std::string::npos &&
            foundationLayer.find("src/text/AssetGuid.cpp") != std::string::npos &&
            foundationLayer.find("src/tooling/Json.cpp") != std::string::npos,
        "Foundation layer must own TypeMetadata.cpp, AssetGuid.cpp, and Json.cpp");

    const std::string bytecodeLayer = findSection("realscript_add_layer_library(realscript_layer_bytecode_format");
    require(
        bytecodeLayer.find("src/debug/DebugInfo.cpp") != std::string::npos,
        "Bytecode format layer must own src/debug/DebugInfo.cpp");

    // 4. Verify DAG link order between slices (foundation < bytecode < {compiler, runtime} < native_backends)
    const std::string bytecodeLink = findSection("target_link_libraries(realscript_slice_bytecode_core");
    require(
        bytecodeLink.find("realscript_slice_foundation_core") != std::string::npos,
        "bytecode slice must link foundation slice");

    const std::string compilerLink = findSection("target_link_libraries(realscript_slice_compiler_core");
    require(
        compilerLink.find("realscript_slice_bytecode_core") != std::string::npos &&
            compilerLink.find("realscript_slice_runtime_core") == std::string::npos,
        "compiler slice must depend on bytecode slice and NOT runtime slice");

    const std::string runtimeLink = findSection("target_link_libraries(realscript_slice_runtime_core");
    require(
        runtimeLink.find("realscript_slice_bytecode_core") != std::string::npos &&
            runtimeLink.find("realscript_slice_compiler_core") == std::string::npos,
        "runtime slice must depend on bytecode slice and NOT compiler slice");

    const std::string nativeLink = findSection("target_link_libraries(realscript_slice_native_backends");
    require(
        nativeLink.find("realscript_slice_runtime_core") != std::string::npos &&
            nativeLink.find("realscript_slice_compiler_core") != std::string::npos,
        "native backends slice must depend on runtime and compiler slices");
#endif
}

void testZeroUpwardL1DependencyLeaksInKernelSources() {
#ifndef REALSCRIPT_SOURCE_DIR
    require(false, "REALSCRIPT_SOURCE_DIR compile definition is required");
#else
    const std::filesystem::path root(REALSCRIPT_SOURCE_DIR);
    const std::vector<std::string> kernelDirs = {
        "text", "diagnostics", "syntax", "semantic", "mir", "optimization",
        "bytecode", "compiler", "runtime", "aot_cpp", "jit", "hot_reload"
    };
    const std::vector<std::string> forbiddenL1Prefixes = {
        "realscript/game/",
        "realscript/tooling/",
        "realscript/debug/Debugger",
        "realscript/debug/DapServer",
        "realscript/reflection/",
        "realscript/plugin/",
    };

    auto scanFile = [&](const std::filesystem::path& filePath, bool allowJsonHelper) {
        std::ifstream in(filePath, std::ios::binary);
        require(static_cast<bool>(in), "unable to scan source file: " + filePath.string());
        std::string line;
        std::size_t lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            if (line.find("#include") == std::string::npos) {
                continue;
            }
            for (const auto& forbidden : forbiddenL1Prefixes) {
                if (line.find(forbidden) != std::string::npos) {
                    if (allowJsonHelper &&
                        line.find("realscript/tooling/Json.h") != std::string::npos) {
                        continue;
                    }
                    require(
                        false,
                        "L0 upward dependency violation in " + filePath.generic_string() +
                            ":" + std::to_string(lineNo) + " -> " + line);
                }
            }
        }
    };

    for (const auto& dirName : kernelDirs) {
        const auto srcDir = root / "src" / dirName;
        if (std::filesystem::exists(srcDir)) {
            const bool allowJson = (dirName == "text" || dirName == "diagnostics" || dirName == "semantic");
            for (const auto& entry : std::filesystem::recursive_directory_iterator(srcDir)) {
                if (!entry.is_regular_file()) continue;
                const auto ext = entry.path().extension().string();
                if (ext == ".cpp" || ext == ".h" || ext == ".hpp") {
                    scanFile(entry.path(), allowJson);
                }
            }
        }

        const auto incDir = root / "include" / "realscript" / dirName;
        if (std::filesystem::exists(incDir)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(incDir)) {
                if (!entry.is_regular_file()) continue;
                scanFile(entry.path(), false);
            }
        }
    }

    // Also check L0 top-level headers and debug/DebugInfo.h
    for (const auto& topHeader : {
             root / "include/realscript/AbiGovernance.h",
             root / "include/realscript/HostAbi.h",
             root / "include/realscript/Version.h",
             root / "include/realscript/debug/DebugInfo.h",
         }) {
        require(std::filesystem::exists(topHeader), "missing L0 header: " + topHeader.string());
        scanFile(topHeader, false);
    }
#endif
}

void testModularRuntimeSplitAndEngineRuntime() {
    // 1. ValueOperations: math type IDs, inline struct slot codec, nested & heap structs
    require(runtime::fixedTypeId() != 0, "fixedTypeId must be non-zero");
    require(runtime::fixed16TypeId() != 0, "fixed16TypeId must be non-zero");
    require(runtime::vec2TypeId() != 0, "vec2TypeId must be non-zero");
    require(runtime::vec3TypeId() != 0, "vec3TypeId must be non-zero");
    require(runtime::quatTypeId() != 0, "quatTypeId must be non-zero");

    const auto defVec3 = runtime::makeDefaultInlineStructValue(runtime::vec3TypeId());
    require(
        defVec3.has_value() && runtime::structValueTypeId(*defVec3) == runtime::vec3TypeId(),
        "makeDefaultInlineStructValue(vec3) failed");

    const auto updatedVec3 = runtime::storeInlineStructField(
        *defVec3, 1, runtime::Value{runtime::FixedValue{65536}});
    require(updatedVec3.has_value(), "storeInlineStructField(vec3, y) failed");
    const auto readVec3Y = runtime::loadInlineStructField(*updatedVec3, 1);
    require(
        readVec3Y.has_value() && std::get<runtime::FixedValue>(*readVec3Y).raw == 65536,
        "loadInlineStructField(vec3, y) mismatch");

    const semantic::SymbolId innerType = semantic::stableTypeId("Kernel.Abi::InnerPair");
    const semantic::SymbolId outerType = semantic::stableTypeId("Kernel.Abi::OuterStruct");
    auto innerStruct = runtime::makeStructValueFromFields(
        innerType,
        std::vector<runtime::Value>{std::int64_t{11}, std::string{"nested-ok"}});
    require(innerStruct.isInline, "2-field struct must pack inline");
    auto outerStruct = runtime::makeStructValueFromFields(
        outerType,
        std::vector<runtime::Value>{true, runtime::Value{innerStruct}});
    require(outerStruct.isInline, "outer struct with nested inline struct must pack inline");
    const auto outerField1 = runtime::structValueFieldAt(outerStruct, 1);
    require(
        outerField1.has_value() && std::get<runtime::StructValue>(*outerField1) == innerStruct,
        "nested inline struct round-trip equality failed");

    const semantic::SymbolId wideType = semantic::stableTypeId("Kernel.Abi::WideStruct");
    auto makeWide = [&]() {
        return runtime::makeStructValueFromFields(
            wideType,
            std::vector<runtime::Value>{
                std::int64_t{1}, std::int64_t{2}, std::int64_t{3},
                std::int64_t{4}, std::int64_t{5}, std::string{"heap-backed"},
            });
    };
    auto wideA = makeWide();
    auto wideB = makeWide();
    require(!wideA.isInline && wideA == wideB, "heap-backed StructValue equality failed");

    // 2. Striped BindingRegistry concurrency & duplicate name guard
    auto bindings = std::make_shared<runtime::BindingRegistry>();
    require(
        bindings->bind(
            "Kernel.Host::ComputeScale",
            [](const bytecode::FunctionReference&,
               runtime::ValueSpan args,
               runtime::RuntimeError&) -> std::optional<runtime::Value> {
                const auto x = std::get<std::int64_t>(args[0]);
                return runtime::Value{x * 3 + 5};
            },
            runtime::BindingDeterminism::Deterministic,
            "Host.Math"),
        "BindingRegistry::bind failed");
    require(
        !bindings->bind(
            "Kernel.Host::ComputeScale",
            [](const bytecode::FunctionReference&,
               runtime::ValueSpan,
               runtime::RuntimeError&) -> std::optional<runtime::Value> {
                return runtime::Value{std::int64_t{0}};
            }),
        "duplicate canonicalName binding must be rejected");
    require(bindings->size() == 1, "BindingRegistry::size() mismatch");

    std::atomic<int> threadFailures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&, t]() {
            for (int iter = 0; iter < 64; ++iter) {
                bytecode::FunctionReference ref{};
                ref.symbolId = static_cast<semantic::SymbolId>(0xABCDEF0000ULL + t * 64 + iter);
                ref.name = "Kernel.Host::ComputeScale";
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
        bindings->cachedSymbolCount() >= 1u + 4u * 64u,
        "BindingRegistry striped cache did not record resolved symbol IDs");

    // 3. ProgramImage::link duplicate rejection & EngineRuntime invocation
    auto modules = compileScriptModules({
        {"core.rscript", R"(
module Kernel.Core;
public interface IScorer {
    int Score(int baseVal);
}
public class BonusScorer : IScorer {
    public int bonus;
    public BonusScorer(int b) { bonus = b; }
    public int Score(int baseVal) { return baseVal * 2 + bonus; }
}
)"},
        {"app.rscript", R"(
module Kernel.App;
import Kernel.Core;
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

    std::vector<bytecode::Module> dupModules = {modules[0], modules[0]};
    runtime::RuntimeError dupErr;
    auto dupLinked = runtime::ProgramImage::link(std::move(dupModules), dupErr);
    require(
        !dupLinked.has_value() && dupErr.code == runtime::ErrorCode::DuplicateSymbol,
        "ProgramImage::link must reject duplicate SymbolIds");

    runtime::EngineRuntime engine(
        std::make_shared<const runtime::ProgramImage>(std::move(*linked)));
    const auto execRes = engine.invoke("Kernel.App::RunPipeline", {std::int64_t{15}});
    require(execRes.succeeded, "EngineRuntime::invoke failed: " + execRes.error.message);
    require(
        std::get<std::int64_t>(execRes.value) == 37,
        "EngineRuntime::invoke returned unexpected value");
    require(
        std::string(runtime::traceEventKindName(runtime::TraceEventKind::FunctionEnter)) ==
            "function-enter",
        "traceEventKindName mismatch");
}

void testAbiContractManifestAndCompatibilityMatrix() {
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
        manifest.rsgsStateVersion == kRsgsStateEncodingVersion &&
            manifest.rsgsStateVersion == 1u,
        "RSGS state encoding version mismatch");
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
    require(
        rs_get_host_api_v1() != nullptr &&
            rs_get_host_api_v1()->struct_size == sizeof(RsHostApiV1) &&
            rs_get_host_api_v1()->abi_major == RS_HOST_ABI_MAJOR_V1,
        "HostAbi v1 descriptor contract mismatch");
    require(manifest.hasValidDigest(), "currentAbiContractManifest digest must be valid");

    const std::string json = manifest.toJson();
    std::string parseErr;
    const auto roundTripped = AbiContractManifest::fromJson(json, &parseErr);
    require(
        roundTripped.has_value() && *roundTripped == manifest && roundTripped->hasValidDigest(),
        "AbiContractManifest JSON round-trip mismatch: " + parseErr);

    std::string badErr;
    require(!AbiContractManifest::fromJson("{ malformed json", &badErr).has_value() && !badErr.empty(),
        "fromJson must reject malformed JSON");
    require(!AbiContractManifest::fromJson("{\"schemaVersion\": 1}", &badErr).has_value(),
        "fromJson must reject incomplete JSON");

    // Compatibility matrix
    require(verifyAbiCompatibility(manifest, manifest).compatible(),
        "identical manifests must be Compatible");

    auto patchSkew = manifest;
    patchSkew.engineVersionPatch += 3;
    patchSkew.refreshLayoutDigest();
    require(verifyAbiCompatibility(manifest, patchSkew).compatible(),
        "patch skew must remain Compatible");

    {
        auto legacyBc = manifest;
        legacyBc.bytecodeTargetVersion = BytecodeVersionPoint{0, 6};
        legacyBc.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(manifest, legacyBc);
        require(
            rep.status == AbiCompatibilityStatus::CompatibleWithMigration &&
                rep.canLoad() && rep.requiresMigration() &&
                rep.hasIssue(AbiIssueCode::BytecodeLegacyMetadataMigration),
            "RSBC 0.6 target must report CompatibleWithMigration");
    }
    {
        auto hostV2State = manifest;
        hostV2State.scriptObjectStateVersion = 2;
        hostV2State.rsgsStateVersion = 2;
        hostV2State.gameSdkPackageVersion = 2;
        hostV2State.nativeModuleAbi.abiMinor = 1;
        hostV2State.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(hostV2State, manifest);
        require(
            rep.status == AbiCompatibilityStatus::CompatibleWithMigration &&
                rep.hasIssue(AbiIssueCode::ScriptObjectStateMigrationRequired) &&
                rep.hasIssue(AbiIssueCode::RsgsStateMigrationRequired) &&
                rep.hasIssue(AbiIssueCode::GameSdkPackageOlderMigration) &&
                rep.hasIssue(AbiIssueCode::NativeModuleAbiMinorBackwardCompatible),
            "older state/package/nativeMinor versions must report CompatibleWithMigration");
    }
    {
        auto tampered = manifest;
        tampered.layoutDigest ^= 0x1ULL;
        const auto rep = verifyAbiCompatibility(manifest, tampered);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible && !rep.canLoad() &&
                rep.hasIssue(AbiIssueCode::LayoutDigestMismatch),
            "tampered layoutDigest must be rejected");
    }
    {
        auto badValueSize = manifest;
        badValueSize.runtimeValueSize = 24;
        badValueSize.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(manifest, badValueSize);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible &&
                rep.hasIssue(AbiIssueCode::RuntimeValueLayoutMismatch),
            "mismatched sizeof(Value) must be rejected");
    }
    {
        auto badNative = manifest;
        badNative.nativeModuleAbi.abiMajor += 1;
        badNative.nativeModuleAbi.moduleExportsStructSize += 8;
        badNative.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(manifest, badNative);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible &&
                rep.hasIssue(AbiIssueCode::NativeModuleAbiMajorMismatch) &&
                rep.hasIssue(AbiIssueCode::NativeModuleAbiStructSizeMismatch),
            "mismatched NativeModuleAbi must be rejected");
    }
    {
        auto futureBc = manifest;
        futureBc.bytecodeMinVersion = BytecodeVersionPoint{0, 10};
        futureBc.bytecodeMaxVersion = BytecodeVersionPoint{0, 12};
        futureBc.bytecodeTargetVersion = BytecodeVersionPoint{0, 10};
        futureBc.refreshLayoutDigest();
        const auto rep = verifyAbiCompatibility(manifest, futureBc);
        require(
            rep.status == AbiCompatibilityStatus::Incompatible &&
                rep.hasIssue(AbiIssueCode::BytecodeVersionRangeDisjoint) &&
                rep.hasIssue(AbiIssueCode::BytecodeTargetVersionUnsupported),
            "disjoint future .rsbc version range must be rejected");
    }
}

void testBytecodeHeaderFastPreflight() {
    auto modules = compileScriptModules({{"probe.rscript", R"(
module Kernel.Probe;
public int Ping() { return 42; }
)"}});
    require(modules.size() == 1, "expected single module");

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

    auto mod09 = modules[0];
    mod09.version = bytecode::Version{0, 9};
    const auto bytes09 = bytecode::encodeModule(mod09);
    const auto rep09 = inspectBytecodeHeaderCompatibility(bytes09);
    require(
        rep09.compatible() && rep09.hasObjectModelSection &&
            rep09.hasSequenceResultMetadata && rep09.hasExceptionMetadata,
        ".rsbc 0.9 header capability flags mismatch");

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

    const std::vector<std::uint8_t> shortBytes{'R', 'S', 'B', 'C', 0, 0};
    require(
        !inspectBytecodeHeaderCompatibility(shortBytes).canLoad() &&
            inspectBytecodeHeaderCompatibility(shortBytes).hasIssue(AbiIssueCode::BytecodeHeaderTruncated),
        "truncated header must be rejected");

    auto badMagic = bytes07;
    badMagic[0] = 'X';
    require(
        !inspectBytecodeHeaderCompatibility(badMagic).canLoad() &&
            inspectBytecodeHeaderCompatibility(badMagic).hasIssue(AbiIssueCode::BytecodeInvalidMagic),
        "bad magic header must be rejected");

    auto unsupported05 = bytes06;
    unsupported05[6] = 5u;
    require(
        !inspectBytecodeHeaderCompatibility(unsupported05).canLoad() &&
            inspectBytecodeHeaderCompatibility(unsupported05).hasIssue(
                AbiIssueCode::BytecodeTargetVersionUnsupported),
        ".rsbc 0.5 header must be rejected");

    auto unsupported010 = bytes09;
    unsupported010[6] = 10u;
    require(
        !inspectBytecodeHeaderCompatibility(unsupported010).canLoad() &&
            inspectBytecodeHeaderCompatibility(unsupported010).hasIssue(
                AbiIssueCode::BytecodeTargetVersionUnsupported),
        ".rsbc 0.10 header must be rejected");

    auto wrongSecCount = bytes07;
    wrongSecCount[12] = 7u;
    require(
        !inspectBytecodeHeaderCompatibility(wrongSecCount).canLoad() &&
            inspectBytecodeHeaderCompatibility(wrongSecCount).hasIssue(
                AbiIssueCode::BytecodeSectionCountMismatch),
        "mismatched section count in .rsbc 0.7 header must be rejected");
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

    run("CMake L0+L1 slice topology and orthogonal apps", testCMakeSliceTopology);
    run("Zero upward L1 #include leaks across L0 sources/headers", testZeroUpwardL1DependencyLeaksInKernelSources);
    run("ValueOperations / BindingRegistry / ProgramImage / EngineRuntime", testModularRuntimeSplitAndEngineRuntime);
    run("AbiContractManifest JSON round-trip & verifyAbiCompatibility matrix", testAbiContractManifestAndCompatibilityMatrix);
    run(".rsbc header compatibility fast preflight", testBytecodeHeaderFastPreflight);

    return failures == 0 ? 0 : 1;
}
