#pragma once

#include "realscript/runtime/FiberScheduler.h"
#include "realscript/runtime/Runtime.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace realscript::runtime {

/**
 * Kernel signature convention for deterministic data-parallel jobs (`P2-C`):
 * - `Element`: `void Kernel(int index, ...)` or `<Scalar> Kernel(int index, ...)`
 * - `Range`:   `int KernelRange(int start, int count, ...)` or `void/<Scalar> KernelRange(int start, int count, ...)`
 * - `Auto`:    Verifies `param[0] == int` and reports compatibility with both `Element` and `Range`.
 */
enum class ParallelKernelKind : std::uint8_t {
    Auto = 0,
    Element = 1,
    Range = 2,
};

/**
 * Exact static bytecode verification failure category for `verifyParallelJobFunction`.
 */
enum class ParallelVerifyViolationKind : std::uint8_t {
    None = 0,
    FunctionNotFound,
    InvalidKernelSignature,
    UnsupportedParameterType,
    NonPackedArrayParameter,
    HeapObjectAllocation,
    HeapArrayAllocation,
    DelegateAllocationOrCombine,
    DelegateInvocation,
    InstanceFieldMutation,
    StaticFieldMutation,
    AsyncOrAwaitOperation,
    StringAllocationOrMutation,
    ExceptionThrow,
    NonPackedArrayElementAccess,
    UntrackedArrayMutation,
    DynamicPolymorphicCall,
    ImpureExternalHostCall,
    ImpureRuntimeIntrinsic,
    NonInlineStructMutation,
};

struct ParallelVerifyViolation {
    semantic::SymbolId functionSymbolId = 0;
    std::string functionName;
    bytecode::BlockId blockId = 0;
    std::uint32_t instructionIndex = 0;
    std::string opcodeName;
    ParallelVerifyViolationKind kind = ParallelVerifyViolationKind::None;
    std::string message;
};

struct ParallelJobVerifyOptions {
    ParallelKernelKind expectedKind = ParallelKernelKind::Auto;
    const BindingRegistry* bindings = nullptr;
    std::unordered_set<semantic::SymbolId> pureExternalSymbols;
    std::unordered_set<std::string> pureExternalNames;
    bool allowBuiltInParallelPrng = true;
};

struct ParallelJobVerificationReport {
    bool valid = false;
    semantic::SymbolId entrySymbolId = 0;
    std::string qualifiedName;
    bool supportsElementKernel = false;
    bool supportsRangeKernel = false;
    semantic::PrimitiveType returnType = semantic::PrimitiveType::Error;
    semantic::SymbolId returnTypeId = 0;
    std::vector<semantic::PrimitiveType> parameterTypes;
    std::vector<semantic::SymbolId> parameterTypeIds;

    // 0-based indices into the kernel's full `parameterTypes` list:
    std::vector<std::size_t> readArrayParamIndices;
    std::vector<std::size_t> writtenArrayParamIndices;
    std::vector<semantic::SymbolId> referencedStaticFieldIds;

    bool usesParallelPrng = false;
    std::size_t verifiedFunctionCount = 0;
    std::vector<ParallelVerifyViolation> violations;

    [[nodiscard]] std::string summaryMessage() const;
};

[[nodiscard]] const char* parallelVerifyViolationKindName(
    ParallelVerifyViolationKind kind) noexcept;

[[nodiscard]] bool isPackedParallelElementType(
    semantic::PrimitiveType elementType) noexcept;

[[nodiscard]] bool isBuiltInParallelPrngFunctionName(
    std::string_view canonicalOrShortName) noexcept;

/**
 * Statically verifies that a target bytecode function and its entire transitive
 * call graph are safe for lock-free, zero-GC multi-core execution.
 */
[[nodiscard]] ParallelJobVerificationReport verifyParallelJobFunction(
    const ProgramImage& program,
    semantic::SymbolId symbolId,
    const ParallelJobVerifyOptions& options = {});

[[nodiscard]] ParallelJobVerificationReport verifyParallelJobFunction(
    const ProgramImage& program,
    const std::string& qualifiedName,
    const ParallelJobVerifyOptions& options = {});

/**
 * Deterministic ordered reduction operator across batches `0 .. numBatches - 1`.
 */
enum class ParallelReduceOp : std::uint8_t {
    None = 0,
    Sum,
    Min,
    Max,
    BitwiseXor,
    BitwiseOr,
    BitwiseAnd,
    Custom,
};

using ParallelCustomReducer = std::function<bool(
    const Value& accumulated,
    const Value& nextValue,
    Value& outReduced,
    RuntimeError& outError)>;

struct ParallelJobConfig {
    std::size_t workerCount = 0; // 0 => hardware_concurrency() (clamped >= 1)
    std::size_t batchSize = 64;
    std::uint32_t baseSeed = 0x53434257u;
    ParallelReduceOp reduceOp = ParallelReduceOp::None;
    ParallelCustomReducer customReducer;
    ExecutionOptions executionOptions{};
    bool allowInPlaceSingleArrayReadWrite = true;

    // Optional explicit 0-based indices into `userArguments` (after index / start,count):
    std::unordered_set<std::size_t> explicitReadOnlyUserArgIndices;
    std::unordered_set<std::size_t> explicitWriteUserArgIndices;

    // Additional pure deterministic host functions allowed during verification:
    std::unordered_set<semantic::SymbolId> pureExternalSymbols;
    std::unordered_set<std::string> pureExternalNames;
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ParallelJobResult {
    bool succeeded = false;
    Value value{};
    RuntimeError error{};
    RuntimeStatistics statistics{};
    std::uint64_t determinismDigest = 0;
    std::size_t numBatches = 0;
    std::size_t effectiveWorkerCount = 0;
    std::size_t totalItemsProcessed = 0;
    std::vector<std::uint64_t> batchDigests;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/**
 * Registers the built-in per-shard deterministic PRNG externals into a `BindingRegistry`
 * (compatible with both `ParallelJobScheduler` multi-core execution and single-threaded fallback):
 * - `<moduleName>::ParallelRandomUInt32() -> int` (non-negative 31-bit or signed 32-bit bits)
 * - `<moduleName>::ParallelRandomInt(int minInclusive, int maxExclusive) -> int`
 * - `<moduleName>::ParallelRandomRange(int minInclusive, int maxInclusive) -> int`
 * - `<moduleName>::ParallelRandomByte255() -> int`
 * - `<moduleName>::ParallelRandomDouble() -> double` (in [0.0, 1.0))
 */
void registerParallelPrngBindings(
    BindingRegistry& registry,
    std::string_view moduleName = {});

/**
 * Multi-core deterministic lock-free shard scheduler (`P2-C`).
 *
 * Guarantees:
 * 1. Pre-validates function safety (`verifyParallelJobFunction`), packed array liveness,
 *    and output/input `ObjectRef` non-aliasing on the main thread before worker dispatch.
 * 2. Enters a zero-GC, zero-mutex `ManagedHeap::ParallelLockFreeScope` and drives
 *    per-worker independent `Interpreter` instances across batches `0 .. numBatches - 1`.
 * 3. Seeds each batch's `DeterministicPRNG` as `baseSeed ^ static_cast<uint32_t>(chunkIndex)`.
 * 4. Deterministically reduces return values, errors, `RuntimeStatistics`, and
 *    `determinismDigest` in fixed batch order `0 .. numBatches - 1`, guaranteeing 100%
 *    bit-identical results across 1, 4, 8, or N worker threads in `DeterminismMode::Strict`.
 */
class ParallelJobScheduler {
public:
    explicit ParallelJobScheduler(std::size_t defaultWorkerCount = 0);
    ~ParallelJobScheduler();

    ParallelJobScheduler(const ParallelJobScheduler&) = delete;
    ParallelJobScheduler& operator=(const ParallelJobScheduler&) = delete;
    ParallelJobScheduler(ParallelJobScheduler&&) noexcept;
    ParallelJobScheduler& operator=(ParallelJobScheduler&&) noexcept;

    void setDefaultWorkerCount(std::size_t workerCount) noexcept;
    [[nodiscard]] std::size_t defaultWorkerCount() const noexcept;

    /**
     * Dispatches an element kernel `Kernel(int index, userArguments...)` over `[0, totalCount)`.
     */
    [[nodiscard]] ParallelJobResult dispatchFor(
        const std::shared_ptr<const ProgramImage>& program,
        const std::shared_ptr<ManagedHeap>& heap,
        semantic::SymbolId symbolId,
        std::size_t totalCount,
        const std::vector<Value>& userArguments = {},
        const ParallelJobConfig& config = {},
        const std::shared_ptr<const BindingRegistry>& bindings = nullptr);

    [[nodiscard]] ParallelJobResult dispatchFor(
        const std::shared_ptr<const ProgramImage>& program,
        const std::shared_ptr<ManagedHeap>& heap,
        const std::string& qualifiedName,
        std::size_t totalCount,
        const std::vector<Value>& userArguments = {},
        const ParallelJobConfig& config = {},
        const std::shared_ptr<const BindingRegistry>& bindings = nullptr);

    /**
     * Dispatches a range kernel `KernelRange(int start, int count, userArguments...)`
     * partitioned into fixed-size batches `[start, start + count)` of size `config.batchSize`.
     */
    [[nodiscard]] ParallelJobResult dispatchRange(
        const std::shared_ptr<const ProgramImage>& program,
        const std::shared_ptr<ManagedHeap>& heap,
        semantic::SymbolId symbolId,
        std::size_t totalCount,
        const std::vector<Value>& userArguments = {},
        const ParallelJobConfig& config = {},
        const std::shared_ptr<const BindingRegistry>& bindings = nullptr);

    [[nodiscard]] ParallelJobResult dispatchRange(
        const std::shared_ptr<const ProgramImage>& program,
        const std::shared_ptr<ManagedHeap>& heap,
        const std::string& qualifiedName,
        std::size_t totalCount,
        const std::vector<Value>& userArguments = {},
        const ParallelJobConfig& config = {},
        const std::shared_ptr<const BindingRegistry>& bindings = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace realscript::runtime
