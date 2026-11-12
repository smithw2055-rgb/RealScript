#pragma once

#include "realscript/bytecode/Bytecode.h"
#include "realscript/compiler/Compilation.h"
#include "realscript/diagnostics/Diagnostic.h"
#include "realscript/optimization/Optimizer.h"
#include "realscript/reflection/Reflection.h"
#include "realscript/runtime/Runtime.h"
#include "realscript/syntax/Syntax.h"
#include "realscript/text/Text.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realscript::plugin {

// ============================================================================
// SemVer & Dependency Version Constraint Solver
// ============================================================================

struct SemVer {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    constexpr SemVer() noexcept = default;
    constexpr SemVer(std::uint32_t maj, std::uint32_t min, std::uint32_t pat) noexcept
        : major(maj), minor(min), patch(pat) {}

    [[nodiscard]] static std::optional<SemVer> parse(std::string_view text) noexcept;
    [[nodiscard]] std::string toString() const;

    friend constexpr bool operator==(SemVer a, SemVer b) noexcept {
        return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
    }
    friend constexpr bool operator!=(SemVer a, SemVer b) noexcept {
        return !(a == b);
    }
    friend constexpr bool operator<(SemVer a, SemVer b) noexcept {
        if (a.major != b.major) return a.major < b.major;
        if (a.minor != b.minor) return a.minor < b.minor;
        return a.patch < b.patch;
    }
    friend constexpr bool operator<=(SemVer a, SemVer b) noexcept {
        return !(b < a);
    }
    friend constexpr bool operator>(SemVer a, SemVer b) noexcept {
        return b < a;
    }
    friend constexpr bool operator>=(SemVer a, SemVer b) noexcept {
        return !(a < b);
    }
};

enum class VersionComparatorOp : std::uint8_t {
    Any,
    Equal,
    NotEqual,
    Greater,
    GreaterOrEqual,
    Less,
    LessOrEqual,
};

struct VersionComparator {
    VersionComparatorOp op = VersionComparatorOp::Any;
    SemVer target{};

    [[nodiscard]] bool matches(SemVer version) const noexcept;
};

struct VersionConstraint {
    std::string rawExpression = "*";
    std::vector<VersionComparator> comparators;
    bool valid = true;

    [[nodiscard]] static std::optional<VersionConstraint> parse(std::string_view expr);
    [[nodiscard]] bool satisfies(SemVer version) const noexcept;
};

struct ModDependency {
    std::string modId;
    VersionConstraint constraint;

    ModDependency() = default;
    ModDependency(std::string id, VersionConstraint c)
        : modId(std::move(id)), constraint(std::move(c)) {}
    ModDependency(std::string id, std::string_view constraintExpr);
    ModDependency(std::string id, const char* constraintExpr)
        : ModDependency(std::move(id), std::string_view(constraintExpr ? constraintExpr : "")) {}
};

struct ModManifest {
    std::string id;
    SemVer version{1, 0, 0};
    std::vector<ModDependency> dependencies;
    std::set<std::string> grantedCapabilities;
    std::size_t maxHeapBytes = 4 * 1024 * 1024;
    std::uint64_t tickInstructionBudget = 100'000;
    std::string entryModule;
};

[[nodiscard]] bool parseModManifestJson(
    const std::string& jsonText,
    ModManifest& outManifest,
    std::string& outError);

[[nodiscard]] std::optional<ModManifest> parseModManifestJson(
    const std::string& jsonText,
    std::string* outError = nullptr);

enum class ModPlanErrorKind : std::uint8_t {
    None = 0,
    InvalidManifest,
    DuplicateModId,
    MissingDependency,
    VersionIncompatible,
    DependencyCycle,
};

struct ModPlanDiagnostic {
    ModPlanErrorKind kind = ModPlanErrorKind::None;
    std::string modId;
    std::string dependencyId;
    std::vector<std::string> cyclePath;
    std::string message;
};

struct ModLoadPlan {
    bool succeeded = false;
    std::vector<std::string> loadOrder;
    std::vector<ModPlanDiagnostic> diagnostics;
};

[[nodiscard]] ModLoadPlan resolveModLoadPlan(const std::vector<ModManifest>& manifests);

// ============================================================================
// Cross-Heap Safe Deep-Copy Value Marshaler
// ============================================================================

enum class MarshalErrorCode : std::uint8_t {
    None = 0,
    CrossHeapReferenceRejected,
    InvalidSourceReference,
    TargetOutOfMemory,
    TypeMismatch,
    DepthLimitExceeded,
};

[[nodiscard]] const char* marshalErrorCodeName(MarshalErrorCode code) noexcept;

struct MarshalResult {
    bool succeeded = false;
    MarshalErrorCode errorCode = MarshalErrorCode::None;
    std::string errorMessage;
    runtime::Value value{};
};

[[nodiscard]] MarshalResult marshalCrossHeapValue(
    const runtime::Value& value,
    const runtime::ManagedHeap* sourceHeap,
    runtime::ManagedHeap* targetHeap,
    std::uint32_t maxDepth = 32);

// ============================================================================
// Compiler Extension Hooks & Capability-Sandboxed ModHost
// ============================================================================

struct ModAstUnit {
    std::string sourcePath;
    std::shared_ptr<text::SourceText> sourceText;
    std::shared_ptr<syntax::CompilationUnitSyntax> syntaxTree;
    std::string moduleName;
    std::vector<std::string> importedModules;
};

struct CompilerExtensionPass {
    std::string name;
    std::function<void(
        const ModManifest& manifest,
        const std::vector<ModAstUnit>& astUnits,
        diagnostics::DiagnosticBag& diagnostics)> onAstParsed;
    std::function<void(
        const ModManifest& manifest,
        const compiler::BuildResult& buildResult,
        diagnostics::DiagnosticBag& diagnostics)> onSemanticBuilt;
};

[[nodiscard]] CompilerExtensionPass makeForbiddenImportPass(
    std::unordered_set<std::string> forbiddenModules,
    std::string passName = "ForbiddenImportGuard");

[[nodiscard]] CompilerExtensionPass makeForbiddenSyntaxPass(
    std::unordered_set<syntax::SyntaxKind> forbiddenSyntaxKinds,
    std::string passName = "ForbiddenSyntaxGuard");

struct ModPackage {
    ModManifest manifest;
    std::vector<compiler::SourceFile> sources;
};

enum class ModState : std::uint8_t {
    Unloaded = 0,
    Active,
    Tripped,
    CompileFailed,
};

struct ModSandboxStats {
    ModState state = ModState::Unloaded;
    std::uint64_t invocations = 0;
    std::uint64_t totalInstructionsExecuted = 0;
    std::uint64_t faultCount = 0;
    std::uint64_t consecutiveFaults = 0;
    std::size_t liveHeapBytes = 0;
    std::size_t peakHeapBytes = 0;
    std::size_t maxHeapBytes = 0;
    runtime::RuntimeError lastError;
};

struct ModLoadReport {
    bool succeeded = false;
    ModLoadPlan plan;
    std::vector<std::string> loadedMods;
    std::map<std::string, diagnostics::DiagnosticBag> compileDiagnostics;
    std::map<std::string, std::string> linkErrors;
};

enum class BusCallStatus : std::uint8_t {
    Succeeded = 0,
    ModNotFound,
    ModTripped,
    EndpointNotFound,
    CrossHeapReferenceRejected,
    MarshalFailed,
    RuntimeFault,
};

struct ModEndpointResult {
    bool succeeded = false;
    BusCallStatus status = BusCallStatus::Succeeded;
    MarshalErrorCode marshalError = MarshalErrorCode::None;
    runtime::Value value{};
    runtime::RuntimeError runtimeError{};
    std::uint64_t instructionsExecuted = 0;
    bool modTrippedAfterCall = false;
    std::string message;
};

struct EventSubscriberOutcome {
    std::string modId;
    std::string handlerName;
    bool succeeded = false;
    bool skippedBecauseTripped = false;
    BusCallStatus status = BusCallStatus::Succeeded;
    runtime::RuntimeError error{};
    std::uint64_t instructionsExecuted = 0;
};

struct EventPublishReport {
    bool succeeded = true;
    MarshalErrorCode marshalError = MarshalErrorCode::None;
    std::size_t deliveredCount = 0;
    std::size_t failedCount = 0;
    std::size_t skippedTrippedCount = 0;
    std::vector<EventSubscriberOutcome> outcomes;
    std::string errorMessage;
};

struct ModHostConfig {
    std::uint32_t faultThreshold = 1;
    optimization::Level optimizationLevel = optimization::Level::None;
    bool mountReflectModuleByDefault = true;
    std::string reflectRequiredCapability = "reflection";
    std::size_t hostBusHeapMaxBytes = 16 * 1024 * 1024;
};

class ModHost {
public:
    explicit ModHost(ModHostConfig config = {});
    ~ModHost();

    ModHost(const ModHost&) = delete;
    ModHost& operator=(const ModHost&) = delete;

    // Host read-only ambient modules (.d.rscript + BindingRegistry) & compiler security passes
    void registerHostModule(
        std::string moduleName,
        compiler::SourceFile declarationStub,
        std::shared_ptr<const runtime::BindingRegistry> bindings = nullptr);

    void addCompilerExtensionPass(CompilerExtensionPass pass);

    // Load and sandbox multiple Mod packages in deterministic topological order
    [[nodiscard]] ModLoadReport loadMods(const std::vector<ModPackage>& packages);
    bool unloadMod(std::string_view modId);
    void unloadAll();

    // Inspection & Circuit Breaker Management
    [[nodiscard]] const std::vector<std::string>& loadOrder() const noexcept {
        return loadOrder_;
    }
    [[nodiscard]] bool hasMod(std::string_view modId) const noexcept;
    [[nodiscard]] bool isModActive(std::string_view modId) const noexcept;
    [[nodiscard]] bool isModTripped(std::string_view modId) const noexcept;
    [[nodiscard]] std::optional<ModSandboxStats> modStats(std::string_view modId) const;
    [[nodiscard]] const ModManifest* modManifest(std::string_view modId) const noexcept;
    [[nodiscard]] std::shared_ptr<runtime::ManagedHeap> modHeap(std::string_view modId) const noexcept;
    [[nodiscard]] std::shared_ptr<const reflection::ReflectionRegistry> modReflection(
        std::string_view modId) const noexcept;
    [[nodiscard]] runtime::EngineRuntime* modRuntime(std::string_view modId) noexcept;
    [[nodiscard]] const runtime::EngineRuntime* modRuntime(std::string_view modId) const noexcept;
    [[nodiscard]] runtime::ManagedHeap& hostHeap() noexcept { return *hostHeap_; }
    [[nodiscard]] const runtime::ManagedHeap& hostHeap() const noexcept { return *hostHeap_; }

    bool resetCircuitBreaker(std::string_view modId);
    void setFaultThreshold(std::uint32_t threshold) noexcept {
        config_.faultThreshold = threshold == 0 ? 1u : threshold;
    }

    // Event Bus & Cross-Mod Service Bus
    bool subscribeEvent(
        std::string_view modId,
        std::string eventTopic,
        std::string handlerQualifiedOrShortName);

    [[nodiscard]] EventPublishReport publishEvent(
        std::string_view eventTopic,
        const std::vector<runtime::Value>& payload = {},
        const runtime::ManagedHeap* sourceHeap = nullptr);

    [[nodiscard]] ModEndpointResult invokeModEndpoint(
        std::string_view targetModId,
        std::string_view endpointName,
        const std::vector<runtime::Value>& arguments = {},
        const runtime::ManagedHeap* callerHeap = nullptr,
        runtime::ManagedHeap* resultTargetHeap = nullptr);

private:
    struct HostAmbientModule {
        std::string moduleName;
        compiler::SourceFile stub;
        std::shared_ptr<const runtime::BindingRegistry> bindings;
    };

    struct EventSubscription {
        std::string modId;
        semantic::SymbolId symbolId = 0;
        std::string qualifiedName;
        std::size_t arity = 0;
    };

    struct SandboxInstance {
        ModManifest manifest;
        ModState state = ModState::Unloaded;
        std::shared_ptr<const runtime::ProgramImage> program;
        std::shared_ptr<runtime::ManagedHeap> heap;
        std::shared_ptr<runtime::BindingRegistry> bindings;
        std::unique_ptr<runtime::EngineRuntime> runtime;
        std::shared_ptr<reflection::ReflectionRegistry> reflection;
        std::unordered_set<std::string> ownedModules;
        std::unordered_map<std::string, semantic::SymbolId> endpoints;
        std::uint64_t invocations = 0;
        std::uint64_t totalInstructions = 0;
        std::uint64_t faultCount = 0;
        std::uint64_t consecutiveFaults = 0;
        runtime::RuntimeError lastError;
    };

    [[nodiscard]] SandboxInstance* findSandbox(std::string_view modId) noexcept;
    [[nodiscard]] const SandboxInstance* findSandbox(std::string_view modId) const noexcept;
    [[nodiscard]] runtime::ExecutionOptions makeSandboxExecutionOptions(
        const ModManifest& manifest) const;
    void recordSandboxFailure(
        SandboxInstance& instance,
        const runtime::RuntimeError& err) noexcept;
    void discoverAttributesAndEndpoints(SandboxInstance& instance);

    ModHostConfig config_;
    std::shared_ptr<runtime::ManagedHeap> hostHeap_;
    std::vector<HostAmbientModule> hostModules_;
    std::vector<CompilerExtensionPass> compilerPasses_;
    std::vector<std::string> loadOrder_;
    std::unordered_map<std::string, std::unique_ptr<SandboxInstance>> sandboxes_;
    std::unordered_map<std::string, std::vector<EventSubscription>> eventBus_;
};

} // namespace realscript::plugin
