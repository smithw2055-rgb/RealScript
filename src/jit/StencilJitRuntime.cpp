#include "JitInternal.h"
#include "../runtime/RuntimeInternal.h"
#include "../runtime/InterpreterInternal.h"

#include <array>
#include <chrono>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif

namespace realscript::jit {
using namespace jit_detail;

namespace {


[[nodiscard]] std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept {
    if (alignment <= 1) return value;
    const std::size_t rem = value % alignment;
    return rem == 0 ? value : (value + (alignment - rem));
}

struct ScopedJitWriteEnable {
    bool active = false;
    explicit ScopedJitWriteEnable(bool isMapJit) noexcept : active(isMapJit) {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        if (active) {
            pthread_jit_write_protect_np(0);
        }
#endif
    }
    ~ScopedJitWriteEnable() noexcept {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        if (active) {
            pthread_jit_write_protect_np(1);
        }
#endif
    }
    ScopedJitWriteEnable(const ScopedJitWriteEnable&) = delete;
    ScopedJitWriteEnable& operator=(const ScopedJitWriteEnable&) = delete;
};

void fillArchTrapPadding(std::uint8_t* dst, std::size_t byteLen) noexcept {
    if (!dst || byteLen == 0) return;
#if defined(__aarch64__) || defined(_M_ARM64)
    // ARM64: fill 4-byte aligned slots with `brk #0` (0xD4200000 little-endian)
    constexpr std::uint32_t kArm64Brk0 = 0xD4200000u;
    std::size_t off = 0;
    while (off + 4u <= byteLen) {
        std::memcpy(dst + off, &kArm64Brk0, 4u);
        off += 4u;
    }
    if (off < byteLen) {
        std::memset(dst + off, 0, byteLen - off);
    }
#else
    std::memset(dst, 0xCC, byteLen);
#endif
}

[[nodiscard]] std::uint8_t* osAllocateCodePages(std::size_t byteSize, bool& outMapJit) noexcept {
    outMapJit = false;
    if (byteSize == 0) return nullptr;
#if defined(__EMSCRIPTEN__)
    (void)byteSize;
    return nullptr;
#elif defined(_WIN32)
    void* mem = VirtualAlloc(nullptr, byteSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return static_cast<std::uint8_t*>(mem);
#else
#if defined(MAP_ANONYMOUS)
    constexpr int kAnonFlag = MAP_ANONYMOUS;
#elif defined(MAP_ANON)
    constexpr int kAnonFlag = MAP_ANON;
#else
    constexpr int kAnonFlag = 0;
#endif
    if (kAnonFlag == 0) return nullptr;
#if defined(__APPLE__) && defined(TARGET_OS_OSX) && TARGET_OS_OSX && (defined(__aarch64__) || defined(__arm64__)) && defined(MAP_JIT)
    // macOS Apple Silicon: prefer MAP_JIT + pthread_jit_write_protect_np hardware APRR W^X
    void* jitMem = mmap(
        nullptr,
        byteSize,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | kAnonFlag | MAP_JIT,
        -1,
        0);
    if (jitMem != MAP_FAILED) {
        outMapJit = true;
        pthread_jit_write_protect_np(1);
        return static_cast<std::uint8_t*>(jitMem);
    }
#endif
    void* mem = mmap(nullptr, byteSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | kAnonFlag, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    return static_cast<std::uint8_t*>(mem);
#endif
}

[[nodiscard]] bool osProtectRxPages(std::uint8_t* base, std::size_t byteSize, bool isMapJit) noexcept {
    if (!base || byteSize == 0) return false;
#if defined(__EMSCRIPTEN__)
    (void)base;
    (void)byteSize;
    (void)isMapJit;
    return false;
#elif defined(_WIN32)
    (void)isMapJit;
    DWORD oldProtect = 0;
    if (VirtualProtect(base, byteSize, PAGE_EXECUTE_READ, &oldProtect) == 0) {
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), base, byteSize);
    return true;
#else
    if (!isMapJit) {
        if (mprotect(base, byteSize, PROT_READ | PROT_EXEC) != 0) {
            return false;
        }
    }
#if defined(__APPLE__)
    sys_icache_invalidate(base, byteSize);
#elif defined(__GNUC__) || defined(__clang__)
    char* begin = reinterpret_cast<char*>(base);
    __builtin___clear_cache(begin, begin + byteSize);
#endif
    return true;
#endif
}

void osFreePages(std::uint8_t* base, std::size_t byteSize) noexcept {
    if (!base || byteSize == 0) return;
#if defined(__EMSCRIPTEN__)
    (void)base;
    (void)byteSize;
#elif defined(_WIN32)
    (void)byteSize;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, byteSize);
#endif
}

} // namespace

ExecutableCodeArena::ExecutableCodeArena(std::size_t defaultBlockBytes)
    : defaultBlockBytes_(defaultBlockBytes == 0 ? 65536 : defaultBlockBytes) {}

ExecutableCodeArena::~ExecutableCodeArena() {
    releaseAll();
}

ExecutableCodeArena::ExecutableCodeArena(ExecutableCodeArena&& other) noexcept
    : blocks_(std::move(other.blocks_)),
      defaultBlockBytes_(other.defaultBlockBytes_),
      allocatedBytes_(other.allocatedBytes_),
      mappedBytes_(other.mappedBytes_),
      sealed_(other.sealed_) {
    other.allocatedBytes_ = 0;
    other.mappedBytes_ = 0;
    other.sealed_ = false;
}

ExecutableCodeArena& ExecutableCodeArena::operator=(ExecutableCodeArena&& other) noexcept {
    if (this != &other) {
        releaseAll();
        blocks_ = std::move(other.blocks_);
        defaultBlockBytes_ = other.defaultBlockBytes_;
        allocatedBytes_ = other.allocatedBytes_;
        mappedBytes_ = other.mappedBytes_;
        sealed_ = other.sealed_;
        other.allocatedBytes_ = 0;
        other.mappedBytes_ = 0;
        other.sealed_ = false;
    }
    return *this;
}

void ExecutableCodeArena::releaseAll() noexcept {
    for (auto& blk : blocks_) {
        osFreePages(blk.base, blk.capacity);
        blk.base = nullptr;
        blk.capacity = 0;
        blk.used = 0;
        blk.executable = false;
    }
    blocks_.clear();
    allocatedBytes_ = 0;
    mappedBytes_ = 0;
    sealed_ = false;
}

std::size_t ExecutableCodeArena::systemPageSize() noexcept {
#if defined(__EMSCRIPTEN__)
    return 65536u;
#elif defined(_WIN32)
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize > 0 ? static_cast<std::size_t>(info.dwPageSize) : 4096u;
#else
    const long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<std::size_t>(ps) : 4096u;
#endif
}

std::uint8_t* ExecutableCodeArena::emit(
    const std::uint8_t* code,
    std::size_t size,
    std::size_t alignment) {
    if (sealed_ || !code || size == 0) return nullptr;
#if defined(__aarch64__) || defined(_M_ARM64)
    const std::size_t align = std::max<std::size_t>(alignUp(alignment, 4u), 4u);
#else
    const std::size_t align = std::max<std::size_t>(alignment, 1);
#endif

    if (!blocks_.empty() && !blocks_.back().executable) {
        auto& cur = blocks_.back();
        const std::size_t alignedOff = alignUp(cur.used, align);
        if (alignedOff <= cur.capacity && size <= cur.capacity - alignedOff) {
            ScopedJitWriteEnable writeGuard(cur.mapJit);
            if (alignedOff > cur.used) {
                fillArchTrapPadding(cur.base + cur.used, alignedOff - cur.used);
            }
            std::uint8_t* dst = cur.base + alignedOff;
            std::memcpy(dst, code, size);
            cur.used = alignedOff + size;
            allocatedBytes_ += size;
            return dst;
        }
    }

    const std::size_t pageSz = systemPageSize();
    const std::size_t allocCap = alignUp(std::max(defaultBlockBytes_, size + align), pageSz);
    bool isMapJit = false;
    std::uint8_t* base = osAllocateCodePages(allocCap, isMapJit);
    if (!base) return nullptr;

    Block blk{};
    blk.base = base;
    blk.capacity = allocCap;
    blk.used = size;
    blk.executable = false;
    blk.mapJit = isMapJit;
    {
        ScopedJitWriteEnable writeGuard(blk.mapJit);
        std::memcpy(blk.base, code, size);
    }
    blocks_.push_back(blk);
    mappedBytes_ += allocCap;
    allocatedBytes_ += size;
    return base;
}

bool ExecutableCodeArena::seal() noexcept {
    if (sealed_) return true;
    const std::size_t pageSz = systemPageSize();
    for (auto& blk : blocks_) {
        if (blk.executable) continue;
        if (blk.used == 0u) {
            blk.executable = true;
            continue;
        }
        if (blk.mapJit) {
            if (blk.used < blk.capacity) {
                ScopedJitWriteEnable writeGuard(true);
                fillArchTrapPadding(blk.base + blk.used, blk.capacity - blk.used);
            }
            if (!osProtectRxPages(blk.base, blk.capacity, true)) {
                return false;
            }
        } else {
            const std::size_t protectBytes = std::min(alignUp(blk.used, pageSz), blk.capacity);
            if (blk.used < protectBytes) {
                fillArchTrapPadding(blk.base + blk.used, protectBytes - blk.used);
            }
            if (!osProtectRxPages(blk.base, protectBytes, false)) {
                return false;
            }
            if (protectBytes < blk.capacity) {
#if defined(_WIN32)
                DWORD oldProtect = 0;
                if (VirtualProtect(
                        blk.base + protectBytes,
                        blk.capacity - protectBytes,
                        PAGE_NOACCESS,
                        &oldProtect) == 0) {
                    return false;
                }
#elif !defined(__EMSCRIPTEN__)
                if (mprotect(
                        blk.base + protectBytes,
                        blk.capacity - protectBytes,
                        PROT_NONE) != 0) {
                    return false;
                }
#endif
            }
        }
        blk.executable = true;
    }
    sealed_ = true;
    return true;
}

bool ExecutableCodeArena::sealed() const noexcept { return sealed_; }
std::size_t ExecutableCodeArena::allocatedBytes() const noexcept { return allocatedBytes_; }
std::size_t ExecutableCodeArena::mappedBytes() const noexcept { return mappedBytes_; }
std::size_t ExecutableCodeArena::blockCount() const noexcept { return blocks_.size(); }

bool ExecutableCodeArena::nativeCodeExecutionSupported() noexcept {
#if !defined(__EMSCRIPTEN__) && (defined(__x86_64__) || defined(_M_X64))
    static const bool supported = []() noexcept {
        ExecutableCodeArena probe(systemPageSize());
        // mov eax, 0x52534a54; ret
        static constexpr std::uint8_t kProbeCode[] = {
            0xB8, 0x54, 0x4A, 0x53, 0x52, 0xC3
        };
        std::uint8_t* entry = probe.emit(kProbeCode, sizeof(kProbeCode), 16);
        if (!entry || !probe.seal()) return false;
        using ProbeFn = std::uint32_t (*)();
        const auto fn = bitCastFnPtr<ProbeFn>(entry);
        return fn() == 0x52534A54u;
    }();
    return supported;
#elif !defined(__EMSCRIPTEN__) && (defined(__aarch64__) || defined(_M_ARM64))
    static const bool supported = []() noexcept {
        ExecutableCodeArena probe(systemPageSize());
        // movz w0, #0x4a54; movk w0, #0x5253, lsl #16; ret
        static constexpr std::uint32_t kProbeWords[] = {
            0x52894A80u,
            0x72AA4A60u,
            0xD65F03C0u,
        };
        std::uint8_t* entry = probe.emit(
            reinterpret_cast<const std::uint8_t*>(kProbeWords),
            sizeof(kProbeWords),
            16);
        if (!entry || !probe.seal()) return false;
        using ProbeFn = std::uint32_t (*)();
        const auto fn = bitCastFnPtr<ProbeFn>(entry);
        return fn() == 0x52534A54u;
    }();
    return supported;
#else
    return false;
#endif
}

// ============================================================================

namespace jit_detail {

// Shared Runtime Error, Array Cache & Slow-Path C++ Helpers
// ============================================================================

void appendJitCallerErrorFrame(
    JitRuntimeSession& session,
    const runtime::ProgramImage::FunctionLocation& location,
    const runtime::detail::ExecutionFunction& execFunc,
    std::uint32_t flatIdx) {
    const runtime::ProgramImage::FunctionLocation* inlinedLoc = nullptr;
    const runtime::detail::ExecutionFunction* inlinedExec = nullptr;
    std::uint32_t callerBlkIdx = 0;
    std::uint32_t callerInstIdx = 0;
    std::uint32_t activeBlkIdx = 0;
    std::uint32_t activeInstIdx = 0;
    std::uint32_t activeSpan = 1;
    bool isTerm = false;

    if (flatIdx < execFunc.flatStream.size() && flatIdx < execFunc.flatCold.size()) {
        const auto& fi = execFunc.flatStream[flatIdx];
        const auto cold = execFunc.resolveCold(flatIdx);
        activeSpan = fi.span;
        isTerm =
            fi.flatOpcode >= runtime::detail::FlatOpcode::TermJumpNoArgs ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperLessLocalRegBranch ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperLessLocalConstBranch ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperLessLocalArrayLengthBranch ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump ||
            fi.flatOpcode == runtime::detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump;
        if (cold.inlinedCallee != nullptr && cold.directExecution != nullptr) {
            inlinedLoc = cold.inlinedCallee;
            inlinedExec = cold.directExecution;
            activeBlkIdx = cold.blockIndex;
            activeInstIdx = cold.instructionIndexInBlock;
            callerBlkIdx = cold.callerBlockIndex;
            callerInstIdx = cold.callerInstIndex;
        } else {
            activeBlkIdx = cold.blockIndex;
            activeInstIdx = cold.instructionIndexInBlock;
        }
    }

    const auto makeLoc = [](
        const runtime::ProgramImage::FunctionLocation& loc,
        const runtime::detail::ExecutionFunction& ef,
        std::uint32_t blkIdx,
        std::uint32_t instIdx,
        std::uint32_t span,
        bool term) {
        runtime::StackTraceLocation out;
        out.function = loc.qualifiedName;
        if (!loc.function || !loc.module) return out;
        out.symbolId = loc.function->symbolId;
        const auto& dbg = loc.function->debugInfo;
        if (dbg.sequencePoints.empty()) return out;

        const debug::SequencePoint* bestSp = nullptr;
        if (blkIdx < ef.blocks.size() && ef.blocks[blkIdx].bytecode != nullptr) {
            const auto blkId = ef.blocks[blkIdx].bytecode->id;
            const std::uint32_t maxIdx = instIdx + (span > 1 ? span - 1 : 0);
            for (const auto& sp : dbg.sequencePoints) {
                if (sp.blockId != blkId) continue;
                if (term && sp.terminator) {
                    bestSp = &sp;
                    break;
                }
                if (!sp.terminator && sp.instructionIndex <= maxIdx) {
                    bestSp = &sp;
                }
            }
        }
        if (!bestSp) bestSp = &dbg.sequencePoints.front();
        for (const auto& src : loc.module->sourceFiles) {
            if (src.id == bestSp->range.fileId) {
                out.sourcePath = src.path;
                break;
            }
        }
        out.sourceLine = static_cast<std::uint32_t>(bestSp->range.start.line + 1);
        out.sourceColumn = static_cast<std::uint32_t>(bestSp->range.start.column + 1);
        return out;
    };

    if (inlinedLoc && inlinedExec) {
        auto calleeTrace = makeLoc(*inlinedLoc, *inlinedExec, activeBlkIdx, activeInstIdx, activeSpan, isTerm);
        session.error.stackTrace.push_back(inlinedLoc->qualifiedName);
        session.error.symbolizedStackTrace.push_back(calleeTrace.format());
        session.error.sourceLocations.push_back(std::move(calleeTrace));

        auto callerTrace = makeLoc(location, execFunc, callerBlkIdx, callerInstIdx, 1, false);
        session.error.stackTrace.push_back(location.qualifiedName);
        session.error.symbolizedStackTrace.push_back(callerTrace.format());
        session.error.sourceLocations.push_back(std::move(callerTrace));
    } else {
        auto trace = makeLoc(location, execFunc, activeBlkIdx, activeInstIdx, activeSpan, isTerm);
        session.error.stackTrace.push_back(location.qualifiedName);
        session.error.symbolizedStackTrace.push_back(trace.format());
        session.error.sourceLocations.push_back(std::move(trace));
    }
}

void populateJitErrorDiagnostics(
    JitRuntimeSession& session,
    const runtime::ProgramImage::FunctionLocation& location,
    const runtime::detail::ExecutionFunction& execFunc,
    std::uint32_t flatIdx,
    runtime::ErrorCode code,
    std::string message) {
    session.error.code = code;
    session.error.message = std::move(message);
    session.error.stackTrace.clear();
    session.error.sourceLocations.clear();
    session.error.symbolizedStackTrace.clear();
    appendJitCallerErrorFrame(session, location, execFunc, flatIdx);
}

[[nodiscard]] std::uint32_t setHelperError(
    JitAbiContext* ctx,
    std::uint32_t flatIdx,
    runtime::ErrorCode code,
    std::string message) noexcept {
    ctx->errorFlatIndex = flatIdx;
    ctx->errorCodeRaw = static_cast<std::uint32_t>(code);
    if (ctx->session && ctx->compiledFn && ctx->compiledFn->location && ctx->compiledFn->execFunc) {
        populateJitErrorDiagnostics(
            *ctx->session,
            *ctx->compiledFn->location,
            *ctx->compiledFn->execFunc,
            flatIdx,
            code,
            std::move(message));
    }
    return static_cast<std::uint32_t>(code);
}

[[nodiscard]] bool bindJitArrayCacheSlot(
    runtime::ManagedHeap* heap,
    const runtime::Value& arrVal,
    JitArrayCacheEntry& ce) noexcept {
    if (!heap || arrVal.rawTag() != runtime::ValueTag::ObjectRef) return false;
    const runtime::ObjectRef ref = arrVal.u_.objectRef;
    std::uint64_t slotAndGen = 0;
    std::memcpy(&slotAndGen, &arrVal, sizeof(slotAndGen));

    runtime::PackedArrayFastView pView{};
    if (heap->tryBindPackedArrayFastView(ref, pView)) {
        ce.slotAndGen = slotAndGen;
        ce.heapId = ref.heapId;
        ce.kind = static_cast<std::uint8_t>(ref.kind);
        ce.isPacked = 1u;
        ce.encoding = static_cast<std::uint8_t>(pView.encoding);
        ce.elementType = static_cast<std::uint8_t>(pView.elementType);
        ce.rawData = pView.rawData;
        ce.length = pView.length;
        ce.flatStride = 0u;
        ce.flatSchemaId = 0u;
        ce.elementTypeId = static_cast<std::uint64_t>(pView.elementTypeId);
        ce.dirtyCards = nullptr;
        ce.blittableSchemaId = pView.blittableSchemaId;
        ce.flatFlags = 0u;
        return true;
    }

    runtime::FlatStructArrayFastView fView{};
    if (heap->tryBindFlatStructArrayFastView(ref, fView)) {
        ce.slotAndGen = slotAndGen;
        ce.heapId = ref.heapId;
        ce.kind = static_cast<std::uint8_t>(ref.kind);
        ce.isPacked = 2u;
        ce.encoding = 0u;
        ce.elementType = static_cast<std::uint8_t>(semantic::PrimitiveType::Struct);
        ce.rawData = fView.data;
        ce.length = fView.length;
        ce.flatStride = fView.flatStride;
        ce.flatSchemaId = fView.flatSchemaId;
        ce.elementTypeId = static_cast<std::uint64_t>(fView.elementTypeId);
        ce.dirtyCards = fView.dirtyCards;
        ce.blittableSchemaId = 0u;
        ce.flatFlags = static_cast<std::uint8_t>(
            (fView.containsManagedReferences ? 1u : 0u) |
            (fView.containsPoolValues ? 2u : 0u) |
            (fView.needsRememberedBarrier ? 4u : 0u));
        return true;
    }

    runtime::ArrayFastView bView{};
    if (heap->tryBindArrayFastView(ref, bView)) {
        ce.slotAndGen = slotAndGen;
        ce.heapId = ref.heapId;
        ce.kind = static_cast<std::uint8_t>(ref.kind);
        ce.isPacked = 0u;
        ce.encoding = 0u;
        ce.elementType = static_cast<std::uint8_t>(bView.elementType);
        ce.rawData = bView.data;
        ce.length = bView.length;
        ce.flatStride = 0u;
        ce.flatSchemaId = 0u;
        ce.elementTypeId = static_cast<std::uint64_t>(bView.elementTypeId);
        ce.dirtyCards = nullptr;
        ce.blittableSchemaId = 0u;
        ce.flatFlags = 0u;
        return true;
    }
    return false;
}

void stepJitGcSafepoint(JitAbiContext* ctx) noexcept {
    if (!ctx || !ctx->session) return;
    auto& session = *ctx->session;
    if (!session.heap || session.limits.gcWorkBudget == 0) return;
    if (session.heap->phase() == runtime::GcPhase::Idle &&
        !session.heap->collectionRequested()) {
        return;
    }
    const bool recordGcNs = (session.telemetry != nullptr);
    const auto gcSliceStart = recordGcNs
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    const auto work = session.heap->step(session.shadowStack, session.limits.gcWorkBudget);
    session.stats.gcWorkPerformed += work;
    invalidateJitArrayCaches(*ctx);
    if (recordGcNs) {
        const auto elapsedNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - gcSliceStart).count());
        session.telemetry->recordGcSliceNanoseconds(elapsedNs);
    }
}

namespace {
struct JitSpanRootGuard {
    runtime::ShadowStack& stack;
    bool active = false;
    JitSpanRootGuard(
        runtime::ShadowStack& s,
        const runtime::Value* ptr,
        std::size_t count) noexcept
        : stack(s) {
        if (ptr != nullptr && count > 0u) {
            stack.pushSpanFrame(runtime::ValueSpan(ptr, count), {}, {});
            active = true;
        }
    }
    ~JitSpanRootGuard() noexcept {
        if (active) {
            stack.popFrame();
        }
    }
    JitSpanRootGuard(const JitSpanRootGuard&) = delete;
    JitSpanRootGuard& operator=(const JitSpanRootGuard&) = delete;
};

void storeJitFlatFrameSlot(
    const runtime::detail::ExecutionFunction& execFunc,
    runtime::Value* frameBase,
    std::uint8_t flatBankId,
    std::size_t frameOffset,
    std::uint16_t dstSlot,
    runtime::Value val) noexcept {
    if (execFunc.hasStructPayloads &&
        flatBankId != 0u &&
        dstSlot < execFunc.slotStructSchemaIds.size() &&
        execFunc.slotStructSchemaIds[dstSlot] != 0u) {
        const std::uint16_t sid = execFunc.slotStructSchemaIds[dstSlot];
        const std::uint16_t payOff = execFunc.slotPayloadOffsets[dstSlot];
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (sc != nullptr &&
            payOff != runtime::detail::InvalidCompactReg &&
            runtime::extractStructLeavesToBuffer(val, *sc, frameBase + payOff)) {
            frameBase[dstSlot] = runtime::StructValue::makeFrameSlice(
                sc->structTypeId,
                flatBankId,
                static_cast<std::uint32_t>(frameOffset + payOff),
                sid);
            return;
        }
    }
    frameBase[dstSlot] = std::move(val);
}

[[nodiscard]] bool sanitizeJitCrossHeapValue(
    runtime::ManagedHeap* callerHeap,
    runtime::ManagedHeap* sourceHeap,
    runtime::CrossHeapAccessPolicy crossPolicy,
    runtime::Value val,
    runtime::Value& outVal,
    runtime::ErrorCode& outErrCode,
    const char*& outErrMsg) noexcept {
    if (!callerHeap || !sourceHeap || sourceHeap == callerHeap ||
        sourceHeap->heapId() == callerHeap->heapId()) {
        outVal = std::move(val);
        return true;
    }
    if (crossPolicy == runtime::CrossHeapAccessPolicy::Deny) {
        outErrCode = runtime::ErrorCode::CrossHeapAccessViolation;
        outErrMsg = "cross-heap access denied by active execution policy";
        return false;
    }
    if (val.rawTag() == runtime::ValueTag::ObjectRef) {
        const auto r = val.u_.objectRef;
        if (r.kind == runtime::ObjectKind::String) {
            if (const auto sv = sourceHeap->stringView(r)) {
                runtime::RuntimeError err;
                if (const auto interned = callerHeap->internString(*sv, &err)) {
                    outVal = runtime::Value{*interned};
                    return true;
                }
                outVal = runtime::Value{runtime::StringValue(std::string(*sv))};
                return true;
            }
            if (crossPolicy == runtime::CrossHeapAccessPolicy::ReadOnlySanitizeStrings) {
                outVal = runtime::Value{runtime::NullString{}};
                return true;
            }
        } else if (crossPolicy == runtime::CrossHeapAccessPolicy::ReadOnlySanitizeStrings) {
            outErrCode = runtime::ErrorCode::CrossHeapAccessViolation;
            outErrMsg = "cross-heap non-string ObjectRef rejected under ReadOnlySanitizeStrings";
            return false;
        }
    }
    outVal = std::move(val);
    return true;
}

runtime::ScriptAllocationSite makeJitAllocSite(
    const JitAbiContext& ctx,
    std::uint32_t flatIdx) noexcept {
    if (!ctx.compiledFn) return {};
    const auto fallbackSym = ctx.compiledFn->symbolId;
    if (!ctx.compiledFn->execFunc) return {fallbackSym, 0};
    const auto& execFunc = *ctx.compiledFn->execFunc;
    if (flatIdx >= execFunc.flatCold.size()) return {fallbackSym, 0};
    const auto cold = execFunc.resolveCold(flatIdx);
    const semantic::SymbolId sym =
        (cold.inlinedCallee && cold.inlinedCallee->function)
            ? cold.inlinedCallee->function->symbolId
            : fallbackSym;
    return {
        sym,
        runtime::packAllocationSiteOffset(cold.blockIndex, cold.instructionIndexInBlock),
    };
}
} // namespace

// Slow-path C++ helper for NewArrayFast
std::uint32_t jitNewArrayHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    const auto cold = execFunc.resolveCold(flatIdx);
    auto& session = *ctx->session;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    std::int64_t lenVal = 0;
    if (!ctx->frameBase[fi.src0Slot].tryGetInt64(lenVal)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array length must be int");
    }
    if (lenVal < 0) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::IndexOutOfRange, "array length cannot be negative");
    }
    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    if (!session.heap) {
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::OutOfMemory, "managed heap is unavailable");
    }

    const auto elemType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1));
    const auto arrayTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
    runtime::Value initVal{};
    semantic::SymbolId elemTid = 0;
    if (fi.aux0Slot == 1) {
        switch (elemType) {
        case semantic::PrimitiveType::Int: initVal.storeInt64(0); break;
        case semantic::PrimitiveType::Float: initVal.storeFloat(0.0f); break;
        case semantic::PrimitiveType::Double: initVal.storeDouble(0.0); break;
        case semantic::PrimitiveType::Long: initVal.storeLong(0); break;
        case semantic::PrimitiveType::Bool: initVal.storeBool(false); break;
        case semantic::PrimitiveType::Byte: initVal = runtime::ByteValue{0}; break;
        case semantic::PrimitiveType::SByte: initVal = runtime::SByteValue{0}; break;
        case semantic::PrimitiveType::Short: initVal = runtime::ShortValue{0}; break;
        case semantic::PrimitiveType::UShort: initVal = runtime::UShortValue{0}; break;
        case semantic::PrimitiveType::UInt: initVal = runtime::UIntValue{0}; break;
        case semantic::PrimitiveType::ULong: initVal = runtime::ULongValue{0}; break;
        case semantic::PrimitiveType::Char: initVal = runtime::CharValue{0}; break;
        case semantic::PrimitiveType::Object:
            initVal = runtime::NullObject{};
            if (cold.bytecode) elemTid = cold.bytecode->elementTypeId;
            break;
        case semantic::PrimitiveType::String:
            initVal = runtime::NullString{};
            break;
        default:
            break;
        }
    } else if (fi.aux0Slot >= 2 && fi.aux0Slot <= 10) {
        if (cold.bytecode) elemTid = cold.bytecode->elementTypeId;
        switch (fi.aux0Slot) {
        case 2: initVal = runtime::FixedValue{}; break;
        case 3: initVal = runtime::Fixed16Value{}; break;
        case 4: initVal = runtime::Vec2Value{}; break;
        case 5: initVal = runtime::Vec3Value{}; break;
        case 6: initVal = runtime::QuatValue{}; break;
        case 7: initVal = runtime::Fixed64Value{}; break;
        case 8: initVal = runtime::Vec2fValue{}; break;
        case 9: initVal = runtime::Vec3fValue{}; break;
        default: initVal = runtime::QuatfValue{}; break;
        }
    } else if (fi.aux0Slot == 11) {
        if (cold.bytecode) elemTid = cold.bytecode->elementTypeId;
        initVal = runtime::Value{};
    } else if (cold.bytecode) {
        elemTid = cold.bytecode->elementTypeId;
        if (elemType == semantic::PrimitiveType::Enum) {
            initVal = runtime::EnumValue{elemTid, 0};
        } else if (elemType == semantic::PrimitiveType::Array) {
            initVal = runtime::NullArray{};
        } else if (elemType == semantic::PrimitiveType::Struct) {
            if (runtime::isBlittableStructType(elemTid) ||
                runtime::findFlatStructSchemaByTypeId(elemTid) != nullptr) {
                initVal = runtime::Value{};
            } else if (auto defStruct = runtime::makeDefaultBlittableStructValue(elemTid)) {
                initVal = std::move(*defStruct);
            }
        }
    }

    const runtime::ScopedAllocationSite allocSite(makeJitAllocSite(*ctx, flatIdx));
    bool slotsMoved = false;
    bool ranGc = false;
    runtime::RuntimeError allocErr;
    const auto allocArray = [&]() {
        bool attemptMoved = false;
        auto res = session.heap->allocateTypedArray(
            arrayTid,
            elemType,
            elemTid,
            static_cast<std::size_t>(lenVal),
            initVal,
            &allocErr,
            &attemptMoved);
        slotsMoved = slotsMoved || attemptMoved;
        return res;
    };
    auto ref = allocArray();
    if (!ref.has_value() &&
        (allocErr.code == runtime::ErrorCode::None ||
         allocErr.code == runtime::ErrorCode::OutOfMemory)) {
        ranGc = true;
        allocErr = {};
        (void)session.heap->collectYoung(session.shadowStack);
        ref = allocArray();
        if (!ref.has_value() &&
            (allocErr.code == runtime::ErrorCode::None ||
             allocErr.code == runtime::ErrorCode::OutOfMemory)) {
            allocErr = {};
            session.heap->collectFull(session.shadowStack);
            ref = allocArray();
        }
    }
    if (!ref.has_value()) {
        ctx->remainingBudget -= opDoneCost;
        if (ranGc) {
            invalidateJitArrayCaches(*ctx);
        }
        const auto errCode = allocErr.code == runtime::ErrorCode::None
            ? runtime::ErrorCode::OutOfMemory
            : allocErr.code;
        return setHelperError(
            ctx,
            flatIdx,
            errCode,
            allocErr.message.empty() ? "managed array allocation failed" : allocErr.message);
    }

    ctx->remainingBudget -= fi.span;
    if (slotsMoved || ranGc) {
        invalidateJitArrayCaches(*ctx);
    }
    ctx->frameBase[fi.dstSlot] = *ref;
    if (session.telemetry != nullptr && session.heap->isPackedArray(*ref)) {
        session.telemetry->noteCompactHeapObject(*ref);
    }
    stepJitGcSafepoint(ctx);
    (void)bindJitArrayCacheSlot(
        session.heap,
        ctx->frameBase[fi.dstSlot],
        ctx->arrayCache[fi.dstSlot & 3u]);
    return 0u;
}

// Slow-path C++ helper for ArrayLengthFast & SuperLessLocalArrayLengthBranch
std::uint32_t jitArrayLengthHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    auto& session = *ctx->session;
    const bool isBranch = (fi.flatOpcode == runtime::detail::FlatOpcode::SuperLessLocalArrayLengthBranch);
    const std::uint16_t arrSlot = isBranch ? fi.src1Slot : fi.src0Slot;
    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    std::int64_t lhsInt = 0;
    if (isBranch && !ctx->frameBase[fi.src0Slot].tryGetInt64(lhsInt)) {
        if (ctx->remainingBudget < 1u) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= 1u;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "expected int local in array length branch");
    }

    const runtime::Value& arrVal = ctx->frameBase[arrSlot];
    if (std::holds_alternative<runtime::NullArray>(arrVal)) {
        const std::uint64_t nullCost = isBranch ? (fi.span - 2u) : opDoneCost;
        if (ctx->remainingBudget < nullCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= nullCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::NullReference, "array length attempted to dereference null");
    }

    auto& ce = ctx->arrayCache[arrSlot & 3u];
    std::int64_t len64 = -1;
    if (session.executionPolicy == nullptr &&
        bindJitArrayCacheSlot(session.heap, arrVal, ce)) {
        len64 = static_cast<std::int64_t>(ce.length);
    } else if (const auto* ref = std::get_if<runtime::ObjectRef>(&arrVal); ref && session.heap) {
        runtime::ManagedHeap* targetHeap = session.executionPolicy
            ? session.executionPolicy->resolveHeap(*ref, session.heap)
            : session.heap;
        if (targetHeap &&
            targetHeap != session.heap &&
            targetHeap->heapId() != session.heap->heapId() &&
            session.executionPolicy != nullptr &&
            session.executionPolicy->crossHeapAccessPolicy(session.heap, targetHeap) ==
                runtime::CrossHeapAccessPolicy::Deny) {
            if (ctx->remainingBudget < opDoneCost) {
                ctx->remainingBudget = 0;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= opDoneCost;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::CrossHeapAccessViolation, "cross-heap array length denied by execution policy");
        }
        if (!targetHeap || !targetHeap->isAlive(*ref)) {
            if (ctx->remainingBudget < opDoneCost) {
                ctx->remainingBudget = 0;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= opDoneCost;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid array reference");
        }
        if (const auto optLen = targetHeap->arrayLength(*ref)) {
            len64 = static_cast<std::int64_t>(*optLen);
        }
    }
    if (len64 < 0) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "expected array operand");
    }

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    ctx->remainingBudget -= fi.span;

    if (!isBranch) {
        ctx->frameBase[fi.dstSlot].storeInt64(len64);
        return 0u;
    }

    bool cond = false;
    switch (fi.binaryOpcode1) {
    case runtime::detail::QuickOpcode::LessInt: cond = (lhsInt < len64); break;
    case runtime::detail::QuickOpcode::LessOrEqualInt: cond = (lhsInt <= len64); break;
    case runtime::detail::QuickOpcode::GreaterInt: cond = (lhsInt > len64); break;
    case runtime::detail::QuickOpcode::GreaterOrEqualInt: cond = (lhsInt >= len64); break;
    case runtime::detail::QuickOpcode::EqualInt: cond = (lhsInt == len64); break;
    case runtime::detail::QuickOpcode::NotEqualInt: cond = (lhsInt != len64); break;
    default:
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "invalid array length comparison");
    }
    ++ctx->branchesTaken;
    ctx->branchCondOut = cond ? 1u : 0u;
    return 0u;
}

// Slow-path C++ helper for LoadElementIntFast & LoadElementTypedFast
std::uint32_t jitLoadElementHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    const auto cold = execFunc.resolveCold(flatIdx);
    auto& session = *ctx->session;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    const runtime::Value& arrVal = ctx->frameBase[fi.src0Slot];
    const runtime::Value& idxVal = ctx->frameBase[fi.src1Slot];

    // Lock-free JitArrayCacheEntry fast path for FlatStruct[] & BlittableStruct[]!
    if (session.executionPolicy == nullptr &&
        fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementTypedFast &&
        execFunc.hasStructPayloads &&
        session.flatBankId != 0u &&
        arrVal.rawTag() == runtime::ValueTag::ObjectRef &&
        fi.dstSlot < execFunc.slotStructSchemaIds.size()) {
        const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
        const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
        if (sid != 0u && payOff != runtime::detail::InvalidCompactReg) {
            auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
            std::uint64_t sg = 0;
            std::memcpy(&sg, &arrVal, sizeof(sg));
            if (sg != ce.slotAndGen || arrVal.u_.objectRef.heapId != ce.heapId) {
                (void)bindJitArrayCacheSlot(session.heap, arrVal, ce);
            }
            std::int64_t fastIdx = 0;
            if (sg == ce.slotAndGen &&
                arrVal.u_.objectRef.heapId == ce.heapId &&
                idxVal.tryGetInt64(fastIdx) &&
                static_cast<std::uint64_t>(fastIdx) < ce.length) {
                const auto expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
                if (ce.elementTypeId == expectedTid) {
                    if (ctx->remainingBudget < fi.span) {
                        ctx->remainingBudget = 0;
                        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                    }
                    const auto uIdx = static_cast<std::size_t>(fastIdx);
                    if (ce.isPacked == 2u && ce.flatSchemaId == sid) {
                        const runtime::Value* elemSrc =
                            static_cast<const runtime::Value*>(ce.rawData) + uIdx * ce.flatStride;
                        std::memcpy(
                            static_cast<void*>(ctx->frameBase + payOff),
                            static_cast<const void*>(elemSrc),
                            static_cast<std::size_t>(ce.flatStride) * sizeof(runtime::Value));
                        ctx->remainingBudget -= fi.span;
                        ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                            expectedTid,
                            session.flatBankId,
                            static_cast<std::uint32_t>(session.frameOffset + payOff),
                            sid);
                        return 0u;
                    }
                    if (ce.isPacked == 1u &&
                        ce.encoding == static_cast<std::uint8_t>(runtime::PackedElemEncoding::BlittableStruct)) {
                        if (const auto* sc = runtime::getFlatStructSchema(sid)) {
                            if (runtime::decodePackedStructToLeaves(
                                    ce.rawData, uIdx, ce.blittableSchemaId, *sc, ctx->frameBase + payOff)) {
                                ctx->remainingBudget -= fi.span;
                                ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                                    expectedTid,
                                    session.flatBankId,
                                    static_cast<std::uint32_t>(session.frameOffset + payOff),
                                    sid);
                                return 0u;
                            }
                        }
                    }
                }
            }
        }
    }

    if (std::holds_alternative<runtime::NullArray>(arrVal)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::NullReference, "array element load attempted to dereference null");
    }

    if (!session.heap || arrVal.rawTag() != runtime::ValueTag::ObjectRef) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element load expected array");
    }

    const runtime::ObjectRef ref = arrVal.u_.objectRef;
    runtime::ManagedHeap* targetHeap = session.executionPolicy
        ? session.executionPolicy->resolveHeap(ref, session.heap)
        : session.heap;
    const bool crossHeap = (targetHeap != nullptr &&
                            targetHeap != session.heap &&
                            targetHeap->heapId() != session.heap->heapId());
    runtime::CrossHeapAccessPolicy crossPolicy = runtime::CrossHeapAccessPolicy::AllowReadWrite;
    if (crossHeap && session.executionPolicy != nullptr) {
        crossPolicy = session.executionPolicy->crossHeapAccessPolicy(session.heap, targetHeap);
        if (crossPolicy == runtime::CrossHeapAccessPolicy::Deny) {
            if (ctx->remainingBudget < opDoneCost) {
                ctx->remainingBudget = 0;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= opDoneCost;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::CrossHeapAccessViolation, "cross-heap array load denied by execution policy");
        }
    }
    if (!targetHeap || !targetHeap->isAlive(ref)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "array element load references an invalid managed array");
    }

    std::int64_t idx = 0;
    if (!idxVal.tryGetInt64(idx)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array index expected int");
    }

    if (session.executionPolicy == nullptr) {
        auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
        (void)bindJitArrayCacheSlot(session.heap, arrVal, ce);
    }

    const auto lengthOpt = targetHeap->arrayLength(ref);
    if (!lengthOpt) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid managed array");
    }
    if (idx < 0 || static_cast<std::uint64_t>(idx) >= *lengthOpt) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
    }

    semantic::PrimitiveType expectedType = semantic::PrimitiveType::Int;
    semantic::SymbolId expectedTid = 0;
    if (fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementTypedFast) {
        expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1));
        expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
    } else if (cold.bytecode) {
        expectedType = cold.bytecode->elementType;
        expectedTid = cold.bytecode->elementTypeId;
    }

    const auto actualElemType = targetHeap->arrayElementType(ref);
    const auto actualElemTid = targetHeap->arrayElementTypeId(ref);
    if (!actualElemType || !actualElemTid ||
        *actualElemType != expectedType || *actualElemTid != expectedTid) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element metadata mismatch");
    }

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }

    // Zero-StructStorage fast path when destination slot is a flat-frame struct slice!
    if (expectedType == semantic::PrimitiveType::Struct &&
        execFunc.hasStructPayloads &&
        session.flatBankId != 0u &&
        fi.dstSlot < execFunc.slotStructSchemaIds.size()) {
        const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
        const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
        const auto* sc = sid != 0u ? runtime::getFlatStructSchema(sid) : nullptr;
        if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg) {
            runtime::FlatStructArrayFastView fView{};
            if ((!crossHeap || crossPolicy == runtime::CrossHeapAccessPolicy::AllowReadWrite) &&
                targetHeap->tryBindFlatStructArrayFastView(ref, fView) &&
                fView.elementTypeId == expectedTid &&
                fView.flatStride == sc->flatSlotCount &&
                fView.flatSchemaId == sid) {
                const runtime::Value* elemSrc =
                    fView.data + static_cast<std::size_t>(idx) * fView.flatStride;
                std::memcpy(
                    static_cast<void*>(ctx->frameBase + payOff),
                    static_cast<const void*>(elemSrc),
                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                    expectedTid,
                    session.flatBankId,
                    static_cast<std::uint32_t>(session.frameOffset + payOff),
                    sid);
                return 0u;
            }
            runtime::PackedArrayFastView pView{};
            if (targetHeap->tryBindPackedArrayFastView(ref, pView) &&
                pView.encoding == runtime::PackedElemEncoding::BlittableStruct &&
                runtime::decodePackedStructToLeaves(
                    pView.rawData,
                    static_cast<std::size_t>(idx),
                    pView.blittableSchemaId,
                    *sc,
                    ctx->frameBase + payOff)) {
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                    expectedTid,
                    session.flatBankId,
                    static_cast<std::uint32_t>(session.frameOffset + payOff),
                    sid);
                return 0u;
            }
        }
    }

    const auto loaded = targetHeap->arrayGet(ref, static_cast<std::size_t>(idx));
    if (!loaded) {
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "array element load failed");
    }

    runtime::Value cleanLoaded = *loaded;
    if (crossHeap) {
        runtime::ErrorCode sanCode = runtime::ErrorCode::CrossHeapAccessViolation;
        const char* sanMsg = "cross-heap array element load rejected";
        if (!sanitizeJitCrossHeapValue(
                session.heap, targetHeap, crossPolicy, std::move(cleanLoaded), cleanLoaded, sanCode, sanMsg)) {
            ctx->remainingBudget -= opDoneCost;
            return setHelperError(ctx, flatIdx, sanCode, sanMsg);
        }
    }

    if (execFunc.hasStructPayloads &&
        session.flatBankId != 0u &&
        fi.dstSlot < execFunc.slotStructSchemaIds.size() &&
        execFunc.slotStructSchemaIds[fi.dstSlot] != 0u) {
        const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
        const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg &&
            runtime::extractStructLeavesToBuffer(cleanLoaded, *sc, ctx->frameBase + payOff)) {
            if (crossHeap) {
                for (std::size_t li = 0; li < sc->flatSlotCount; ++li) {
                    runtime::ErrorCode sanCode = runtime::ErrorCode::CrossHeapAccessViolation;
                    const char* sanMsg = "cross-heap struct leaf rejected";
                    if (!sanitizeJitCrossHeapValue(
                            session.heap,
                            targetHeap,
                            crossPolicy,
                            std::move(ctx->frameBase[payOff + li]),
                            ctx->frameBase[payOff + li],
                            sanCode,
                            sanMsg)) {
                        ctx->remainingBudget -= opDoneCost;
                        return setHelperError(ctx, flatIdx, sanCode, sanMsg);
                    }
                }
            }
            ctx->remainingBudget -= fi.span;
            ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                sc->structTypeId,
                session.flatBankId,
                static_cast<std::uint32_t>(session.frameOffset + payOff),
                sid);
            return 0u;
        }
    }
    ctx->remainingBudget -= fi.span;
    ctx->frameBase[fi.dstSlot] = std::move(cleanLoaded);
    return 0u;
}

namespace {
[[nodiscard]] bool jitRuntimeAssignable(
    JitRuntimeSession& session,
    semantic::SymbolId actualTid,
    semantic::SymbolId targetTid) noexcept;
} // namespace

// Slow-path C++ helper for StoreElementIntFast, StoreElementPrimFast & StoreElementObjFast
std::uint32_t jitStoreElementHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    const auto cold = execFunc.resolveCold(flatIdx);
    auto& session = *ctx->session;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    const runtime::Value& arrVal = ctx->frameBase[fi.src0Slot];
    const runtime::Value& idxVal = ctx->frameBase[fi.src1Slot];
    const runtime::Value& storeVal = ctx->frameBase[fi.src2Slot];

    // Lock-free JitArrayCacheEntry fast path for FlatStruct[] & BlittableStruct[] stores!
    if (session.executionPolicy == nullptr &&
        (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFieldInPlace ||
         fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFast) &&
        arrVal.rawTag() == runtime::ValueTag::ObjectRef) {
        auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
        std::uint64_t sg = 0;
        std::memcpy(&sg, &arrVal, sizeof(sg));
        if (sg != ce.slotAndGen || arrVal.u_.objectRef.heapId != ce.heapId) {
            (void)bindJitArrayCacheSlot(session.heap, arrVal, ce);
        }
        std::int64_t fastIdx = 0;
        if (sg == ce.slotAndGen &&
            arrVal.u_.objectRef.heapId == ce.heapId &&
            idxVal.tryGetInt64(fastIdx) &&
            static_cast<std::uint64_t>(fastIdx) < ce.length) {
            const auto uIdx = static_cast<std::size_t>(fastIdx);
            const runtime::ObjectRef ref = arrVal.u_.objectRef;
            if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFieldInPlace) {
                const auto expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
                const auto rootSid = static_cast<std::uint16_t>(fi.auxImm32);
                if (ce.elementTypeId == expectedTid) {
                    if (ce.isPacked == 2u && ce.flatSchemaId == rootSid && fi.aux0Slot < ce.flatStride) {
                        if (ctx->remainingBudget < fi.span) {
                            ctx->remainingBudget = 0;
                            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                        }
                        runtime::Value* dstLeaf =
                            static_cast<runtime::Value*>(ce.rawData) + uIdx * ce.flatStride + fi.aux0Slot;
                        bool wroteOk = false;
                        bool needPoolFallback = false;
                        if (fi.aux1Slot == 0u) {
                            const runtime::Value detached = runtime::detachFrameSliceValue(storeVal);
                            if ((ce.flatFlags & 2u) != 0u &&
                                (detached.rawTag() == runtime::ValueTag::String ||
                                 detached.rawTag() == runtime::ValueTag::QuatArena ||
                                 detached.rawTag() == runtime::ValueTag::QuatfArena)) {
                                needPoolFallback = true;
                            } else {
                                *dstLeaf = detached;
                                wroteOk = true;
                            }
                        } else if ((ce.flatFlags & 2u) == 0u) {
                            const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot);
                            if (subSc != nullptr &&
                                fi.aux0Slot + subSc->flatSlotCount <= ce.flatStride &&
                                runtime::extractStructLeavesToBuffer(storeVal, *subSc, dstLeaf)) {
                                wroteOk = true;
                            }
                        }
                        if (wroteOk && !needPoolFallback) {
                            if ((ce.flatFlags & 1u) != 0u) {
                                if (ce.dirtyCards != nullptr) {
                                    ce.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                                }
                                if ((ce.flatFlags & 4u) != 0u ||
                                    (session.heap != nullptr && session.heap->phase() != runtime::GcPhase::Idle)) {
                                    session.heap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                                    ce.flatFlags = static_cast<std::uint8_t>(ce.flatFlags & ~4u);
                                }
                            }
                            ctx->remainingBudget -= fi.span;
                            return 0u;
                        }
                    } else if (ce.isPacked == 1u &&
                               ce.encoding == static_cast<std::uint8_t>(runtime::PackedElemEncoding::BlittableStruct)) {
                        if (const auto* rootSc = runtime::getFlatStructSchema(rootSid)) {
                            if (ctx->remainingBudget < fi.span) {
                                ctx->remainingBudget = 0;
                                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                            }
                            bool needsDemote = false;
                            if (runtime::writePackedStructLeafSlice(
                                    ce.rawData, uIdx, ce.blittableSchemaId, *rootSc,
                                    fi.aux0Slot, fi.aux1Slot, storeVal, needsDemote)) {
                                ctx->remainingBudget -= fi.span;
                                return 0u;
                            }
                        }
                    }
                }
            } else if (ce.isPacked == 2u && (ce.flatFlags & 2u) == 0u) {
                const auto expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
                if (ce.elementTypeId == expectedTid &&
                    runtime::valueType(storeVal) == semantic::PrimitiveType::Struct) {
                    if (const auto* sc = runtime::getFlatStructSchema(ce.flatSchemaId)) {
                        if (ctx->remainingBudget < fi.span) {
                            ctx->remainingBudget = 0;
                            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                        }
                        runtime::Value* dstElem =
                            static_cast<runtime::Value*>(ce.rawData) + uIdx * ce.flatStride;
                        if (runtime::extractStructLeavesToBuffer(storeVal, *sc, dstElem)) {
                            if ((ce.flatFlags & 1u) != 0u) {
                                if (ce.dirtyCards != nullptr) {
                                    ce.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                                }
                                if ((ce.flatFlags & 4u) != 0u ||
                                    (session.heap != nullptr && session.heap->phase() != runtime::GcPhase::Idle)) {
                                    session.heap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                                    ce.flatFlags = static_cast<std::uint8_t>(ce.flatFlags & ~4u);
                                }
                            }
                            ctx->remainingBudget -= fi.span;
                            return 0u;
                        }
                    }
                }
            }
        }
    }

    if (std::holds_alternative<runtime::NullArray>(arrVal)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::NullReference, "array element store attempted to dereference null");
    }

    if (!session.heap || arrVal.rawTag() != runtime::ValueTag::ObjectRef) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element store expected array");
    }

    const runtime::ObjectRef ref = arrVal.u_.objectRef;
    runtime::ManagedHeap* const targetHeap = session.executionPolicy
        ? session.executionPolicy->resolveHeap(ref, session.heap)
        : session.heap;
    if (!targetHeap || !targetHeap->isAlive(ref)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "array element store references an invalid managed array");
    }

    std::int64_t idx = 0;
    if (!idxVal.tryGetInt64(idx)) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array index expected int");
    }

    const auto lengthOpt = targetHeap->arrayLength(ref);
    if (!lengthOpt) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid managed array");
    }
    if (idx < 0 || static_cast<std::uint64_t>(idx) >= *lengthOpt) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::IndexOutOfRange, "array element index is out of range");
    }

    if (session.executionPolicy != nullptr) {
        if (targetHeap != session.heap &&
            session.executionPolicy->crossHeapAccessPolicy(session.heap, targetHeap) !=
                runtime::CrossHeapAccessPolicy::AllowReadWrite) {
            if (ctx->remainingBudget < opDoneCost) {
                ctx->remainingBudget = 0;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= opDoneCost;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::CrossHeapAccessViolation, "cross-heap array write rejected by execution policy");
        }
        runtime::RuntimeError policyErr;
        const runtime::Value policyVal = runtime::detachFrameSliceValue(storeVal);
        if (!session.executionPolicy->checkArrayWrite(session.heap, targetHeap, ref, policyVal, policyErr)) {
            if (ctx->remainingBudget < opDoneCost) {
                ctx->remainingBudget = 0;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= opDoneCost;
            const auto code = policyErr.code == runtime::ErrorCode::None
                ? runtime::ErrorCode::CapabilityDenied
                : policyErr.code;
            return setHelperError(ctx, flatIdx, code, policyErr.message.empty() ? "array element write rejected by active execution policy" : policyErr.message);
        }
    }

    if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFieldInPlace) {
        if (ctx->remainingBudget < fi.span) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        const auto expectedTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        const auto rootSid = static_cast<std::uint16_t>(fi.auxImm32);
        const auto* rootSc = runtime::getFlatStructSchema(rootSid);
        const auto uIdx = static_cast<std::size_t>(idx);
        runtime::PackedArrayFastView pView{};
        if (rootSc != nullptr &&
            targetHeap->tryBindPackedArrayFastView(ref, pView) &&
            pView.encoding == runtime::PackedElemEncoding::BlittableStruct &&
            pView.elementTypeId == expectedTid) {
            bool needsDemote = false;
            if (runtime::writePackedStructLeafSlice(
                    pView.rawData, uIdx, pView.blittableSchemaId, *rootSc,
                    fi.aux0Slot, fi.aux1Slot, storeVal, needsDemote)) {
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
            // Demote packed array via full element reconstruct + arraySet if int32 overflowed
            if (auto elemOpt = targetHeap->arrayGet(ref, uIdx); elemOpt.has_value()) {
                std::vector<runtime::Value> tmpLeaves(rootSc->flatSlotCount);
                if (runtime::extractStructLeavesToBuffer(*elemOpt, *rootSc, tmpLeaves.data())) {
                    if (fi.aux1Slot == 0u) {
                        tmpLeaves[fi.aux0Slot] = runtime::detachFrameSliceValue(storeVal);
                    } else if (const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot)) {
                        (void)runtime::extractStructLeavesToBuffer(storeVal, *subSc, tmpLeaves.data() + fi.aux0Slot);
                    }
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(*rootSc, tmpLeaves.data());
                    runtime::RuntimeError demErr;
                    const std::uint64_t epochBefore = targetHeap->gcEpoch();
                    if (targetHeap->arraySet(ref, uIdx, rebuilt, &demErr)) {
                        ctx->remainingBudget -= fi.span;
                        if (targetHeap->gcEpoch() != epochBefore) {
                            invalidateJitArrayCaches(*ctx);
                        }
                        if (targetHeap == session.heap) {
                            (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
                        }
                        return 0u;
                    }
                }
            }
        }
        runtime::FlatStructArrayFastView fView{};
        if (rootSc != nullptr &&
            (targetHeap->tryBindFlatStructArrayFastView(ref, fView) && fView.elementTypeId == expectedTid && fView.flatStride == rootSc->flatSlotCount) &&
            fView.flatSchemaId == rootSid &&
            fi.aux0Slot < fView.flatStride) {
            runtime::Value* dstLeaf = fView.data + uIdx * fView.flatStride + fi.aux0Slot;
            if (fi.aux1Slot == 0u) {
                *dstLeaf = runtime::detachFrameSliceValue(storeVal);
                if (fView.containsManagedReferences) {
                    if (fView.dirtyCards != nullptr) {
                        fView.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (fView.needsRememberedBarrier || targetHeap->phase() != runtime::GcPhase::Idle) {
                        targetHeap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                    }
                }
                // If containsPoolValues, delegate via arraySet only when value is pool-backed String/Arena
                if (fView.containsPoolValues &&
                    (dstLeaf->rawTag() == runtime::ValueTag::String ||
                     dstLeaf->rawTag() == runtime::ValueTag::QuatArena ||
                     dstLeaf->rawTag() == runtime::ValueTag::QuatfArena)) {
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(
                        *rootSc, fView.data + uIdx * fView.flatStride);
                    (void)targetHeap->arraySet(ref, uIdx, rebuilt, nullptr);
                    invalidateJitArrayCaches(*ctx);
                    if (targetHeap == session.heap) {
                        (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
                    }
                }
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
            const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot);
            if (subSc != nullptr &&
                fi.aux0Slot + subSc->flatSlotCount <= fView.flatStride &&
                runtime::extractStructLeavesToBuffer(storeVal, *subSc, dstLeaf)) {
                if (fView.containsManagedReferences) {
                    if (fView.dirtyCards != nullptr) {
                        fView.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (fView.needsRememberedBarrier || targetHeap->phase() != runtime::GcPhase::Idle) {
                        targetHeap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                    }
                }
                if (fView.containsPoolValues) {
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(
                        *rootSc, fView.data + uIdx * fView.flatStride);
                    (void)targetHeap->arraySet(ref, uIdx, rebuilt, nullptr);
                    invalidateJitArrayCaches(*ctx);
                    if (targetHeap == session.heap) {
                        (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
                    }
                }
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "in-place struct element field store failed");
    }

    semantic::PrimitiveType expectedType = semantic::PrimitiveType::Int;
    semantic::SymbolId expectedElemTid = 0;
    if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementPrimFast ||
        fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementObjFast) {
        expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1));
        if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementObjFast) {
            expectedElemTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        }
    } else if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFast) {
        expectedType = semantic::PrimitiveType::Struct;
    } else if (cold.bytecode) {
        expectedType = cold.bytecode->elementType;
        expectedElemTid = cold.bytecode->elementTypeId;
    }

    const bool nullMatchesExpected =
        (expectedType == semantic::PrimitiveType::Object && std::holds_alternative<runtime::NullObject>(storeVal)) ||
        (expectedType == semantic::PrimitiveType::String && std::holds_alternative<runtime::NullString>(storeVal)) ||
        (expectedType == semantic::PrimitiveType::Array && std::holds_alternative<runtime::NullArray>(storeVal));
    if (!nullMatchesExpected && runtime::valueType(storeVal) != expectedType) {
        if (ctx->remainingBudget < opDoneCost) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element store value type mismatch");
    }

    if (expectedType == semantic::PrimitiveType::Object) {
        const auto arrElemType = targetHeap->arrayElementType(ref);
        const auto arrElemTid = targetHeap->arrayElementTypeId(ref);
        if (!arrElemType || !arrElemTid ||
            *arrElemType != expectedType ||
            (expectedElemTid != 0 && *arrElemTid != expectedElemTid)) {
            if (ctx->remainingBudget < opDoneCost) {
                ctx->remainingBudget = 0;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= opDoneCost;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "array element metadata does not match the bytecode operation");
        }
        if (!std::holds_alternative<runtime::NullObject>(storeVal)) {
            runtime::ManagedHeap* const valHeap =
                (storeVal.rawTag() == runtime::ValueTag::ObjectRef && session.executionPolicy != nullptr)
                    ? session.executionPolicy->resolveHeap(storeVal.u_.objectRef, session.heap)
                    : session.heap;
            const auto actualTid =
                (storeVal.rawTag() == runtime::ValueTag::ObjectRef && valHeap != nullptr)
                    ? valHeap->objectTypeId(storeVal.u_.objectRef)
                    : std::nullopt;
            if (!actualTid || !jitRuntimeAssignable(session, *actualTid, *arrElemTid)) {
                if (ctx->remainingBudget < opDoneCost) {
                    ctx->remainingBudget = 0;
                    return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                }
                ctx->remainingBudget -= opDoneCost;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "managed array object element type mismatch");
            }
        }
    }

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }

    // Note: ManagedHeap::arraySet already handles FrameSlice StructValue with ZERO StructStorage allocations!
    runtime::RuntimeError setErr;
    const std::uint64_t epochBefore = targetHeap->gcEpoch();
    const auto storePolicy = (expectedType == semantic::PrimitiveType::Object)
        ? runtime::ArrayStoreTypePolicy::PrevalidatedAssignableObject
        : runtime::ArrayStoreTypePolicy::Exact;
    if (!targetHeap->arraySet(ref, static_cast<std::size_t>(idx), storeVal, &setErr, storePolicy)) {
        ctx->remainingBudget -= opDoneCost;
        const auto code = setErr.code == runtime::ErrorCode::None
            ? runtime::ErrorCode::TypeMismatch
            : setErr.code;
        return setHelperError(ctx, flatIdx, code, setErr.message.empty() ? "arraySet failed" : setErr.message);
    }

    ctx->remainingBudget -= fi.span;
    if (targetHeap->gcEpoch() != epochBefore) {
        invalidateJitArrayCaches(*ctx);
    }
    // Re-bind cache slot in case PackedI32 widened to PackedI64 or GC ran
    if (targetHeap == session.heap) {
        (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
    }
    return 0u;
}

bool jitValuesEqual(
    runtime::ManagedHeap* heap,
    const runtime::Value& left,
    const runtime::Value& right,
    bool& outEqual,
    runtime::ErrorCode& outErrCode,
    const char*& outErrMsg) noexcept {
    const auto leftType = runtime::valueType(left);
    if (leftType != runtime::valueType(right)) {
        outEqual = false;
        return true;
    }
    if (leftType == semantic::PrimitiveType::Object ||
        leftType == semantic::PrimitiveType::Array) {
        const auto isValidRef = [&](const runtime::Value& value) noexcept {
            if (value.rawTag() != runtime::ValueTag::ObjectRef) return true;
            const auto& ref = value.asObjectRefUnchecked();
            if (ref.isStackSlotRef()) return true;
            return heap != nullptr && heap->isAlive(ref);
        };
        if (!isValidRef(left) || !isValidRef(right)) {
            outErrCode = runtime::ErrorCode::InvalidObjectReference;
            outErrMsg = "reference equality encountered an invalid managed reference";
            return false;
        }
        outEqual = (left == right);
        return true;
    }
    if (leftType == semantic::PrimitiveType::String) {
        if (std::holds_alternative<runtime::NullString>(left) ||
            std::holds_alternative<runtime::NullString>(right)) {
            outEqual = std::holds_alternative<runtime::NullString>(left) &&
                std::holds_alternative<runtime::NullString>(right);
            return true;
        }
        try {
            const auto extractText = [&](const runtime::Value& value)
                -> std::optional<std::string_view> {
                if (const auto* text = std::get_if<std::string>(&value)) return *text;
                if (value.rawTag() == runtime::ValueTag::String) {
                    return std::string_view(value.asStringUnchecked().str());
                }
                if (const auto* ref = std::get_if<runtime::ObjectRef>(&value)) {
                    return heap ? heap->stringView(*ref) : std::nullopt;
                }
                return std::nullopt;
            };
            const auto a = extractText(left);
            const auto b = extractText(right);
            if (!a || !b) {
                outErrCode = runtime::ErrorCode::InvalidObjectReference;
                outErrMsg = "string equality encountered an invalid managed reference";
                return false;
            }
            outEqual = (*a == *b);
            return true;
        } catch (...) {
            outErrCode = runtime::ErrorCode::InvalidObjectReference;
            outErrMsg = "string equality encountered an invalid managed reference";
            return false;
        }
    }
    if (leftType == semantic::PrimitiveType::Struct) {
        if (left == right) {
            outEqual = true;
            return true;
        }
        if (left.rawTag() == runtime::ValueTag::Struct &&
            right.rawTag() == runtime::ValueTag::Struct) {
            const auto& ls = left.asStructUnchecked();
            const auto& rs = right.asStructUnchecked();
            if (ls.typeId == rs.typeId && ls.fieldCount() == rs.fieldCount()) {
                const auto count = ls.fieldCount();
                for (std::size_t i = 0; i < count; ++i) {
                    const auto lf = runtime::structValueFieldAt(ls, i);
                    const auto rf = runtime::structValueFieldAt(rs, i);
                    if (!lf || !rf) {
                        outEqual = false;
                        return true;
                    }
                    bool fieldEq = false;
                    if (!jitValuesEqual(heap, *lf, *rf, fieldEq, outErrCode, outErrMsg)) {
                        return false;
                    }
                    if (!fieldEq) {
                        outEqual = false;
                        return true;
                    }
                }
                outEqual = true;
                return true;
            }
        }
        outEqual = false;
        return true;
    }
    outEqual = (left == right);
    return true;
}

namespace {

[[nodiscard]] const runtime::detail::PrelinkedDispatchTarget* resolveJitPolymorphicTarget(
    const runtime::detail::ExecutionMirror& mirror,
    semantic::SymbolId actualTypeId,
    const runtime::detail::PolymorphicCallSiteDesc& site) noexcept {
    if (!site.valid || actualTypeId == 0) return nullptr;
    const auto* metaPtr = runtime::detail::resolveTypeDispatch(mirror, actualTypeId);
    if (!metaPtr) return nullptr;
    const auto& meta = *metaPtr;
    if (!site.isInterface) {
        if (site.slot >= meta.virtualTargets.size()) return nullptr;
        const auto& target = meta.virtualTargets[site.slot];
        return (target.target && target.target->function) ? &target : nullptr;
    }
    const auto& imaps = meta.interfaceMaps;
    if (imaps.size() <= 4) {
        for (const auto& m : imaps) {
            if (m.interfaceTypeId == site.interfaceTypeId) {
                if (site.slot >= m.slots.size()) return nullptr;
                const auto& target = m.slots[site.slot];
                return (target.target && target.target->function) ? &target : nullptr;
            }
        }
        return nullptr;
    }
    std::size_t lo = 0;
    std::size_t hi = imaps.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) >> 1u);
        if (imaps[mid].interfaceTypeId < site.interfaceTypeId) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < imaps.size() && imaps[lo].interfaceTypeId == site.interfaceTypeId) {
        if (site.slot >= imaps[lo].slots.size()) return nullptr;
        const auto& target = imaps[lo].slots[site.slot];
        return (target.target && target.target->function) ? &target : nullptr;
    }
    return nullptr;
}

[[nodiscard]] std::optional<runtime::ObjectRef> jitInternStringWithOomRetry(
    JitRuntimeSession& session,
    std::string_view text,
    runtime::RuntimeError& err,
    bool& outRanGc) {
    outRanGc = false;
    err = {};
    auto interned = session.heap->internString(text, &err);
    if (!interned.has_value() &&
        (err.code == runtime::ErrorCode::None ||
         err.code == runtime::ErrorCode::OutOfMemory)) {
        outRanGc = true;
        err = {};
        (void)session.heap->collectYoung(session.shadowStack);
        interned = session.heap->internString(text, &err);
        if (!interned.has_value() &&
            (err.code == runtime::ErrorCode::None ||
             err.code == runtime::ErrorCode::OutOfMemory)) {
            err = {};
            session.heap->collectFull(session.shadowStack);
            interned = session.heap->internString(text, &err);
        }
    }
    return interned;
}

[[nodiscard]] bool jitRuntimeAssignable(
    JitRuntimeSession& session,
    semantic::SymbolId actualTid,
    semantic::SymbolId targetTid) noexcept {
    if (targetTid == 0 || actualTid == targetTid) {
        return true;
    }
    if (actualTid == 0 || !session.program) {
        return false;
    }
    auto& cache = session.assignableCache[(actualTid ^ (targetTid >> 3u)) & 7u];
    if (cache.subTypeId == actualTid && cache.targetTypeId == targetTid) {
        return cache.assignable;
    }
    bool match = false;
    auto curTid = actualTid;
    for (std::size_t depth = 0; curTid != 0 && depth < 256u; ++depth) {
        const auto* t = session.program->resolveType(curTid);
        if (!t) break;
        for (const auto& iface : t->interfaceDispatchMaps) {
            if (iface.interfaceTypeId == targetTid) {
                match = true;
                break;
            }
        }
        if (match || t->baseTypeId == targetTid) {
            match = true;
            break;
        }
        curTid = t->baseTypeId;
    }
    cache.subTypeId = actualTid;
    cache.targetTypeId = targetTid;
    cache.assignable = match;
    return match;
}

[[nodiscard]] bool jitCheckCrossHeapRef(
    JitRuntimeSession& session,
    runtime::ObjectRef ref,
    runtime::ManagedHeap*& outHeap,
    runtime::ErrorCode& errCode,
    const char*& errMsg) noexcept {
    outHeap = session.executionPolicy
        ? session.executionPolicy->resolveHeap(ref, session.heap)
        : session.heap;
    if (!outHeap) {
        errCode = runtime::ErrorCode::InvalidObjectReference;
        errMsg = "missing heap for object reference";
        return false;
    }
    if (session.executionPolicy != nullptr &&
        outHeap != session.heap &&
        outHeap->heapId() != session.heap->heapId() &&
        session.executionPolicy->crossHeapAccessPolicy(session.heap, outHeap) ==
            runtime::CrossHeapAccessPolicy::Deny) {
        errCode = runtime::ErrorCode::CapabilityDenied;
        errMsg = "cross-heap access denied by execution policy";
        return false;
    }
    return true;
}

const std::shared_ptr<const std::vector<std::size_t>>& jitDelegateRefFields() {
    static const auto refs = std::make_shared<const std::vector<std::size_t>>(
        std::initializer_list<std::size_t>{0u, 7u});
    return refs;
}

[[nodiscard]] inline bool jitIsValidDelegateFlags(std::int64_t flags, bool allowMulticast) noexcept {
    return (flags >= 0 && flags <= 2) || (allowMulticast && flags == 4);
}

[[nodiscard]] bool jitReadDelegateIdFromView(
    const runtime::ObjectFastView& view,
    std::size_t lowIndex,
    semantic::SymbolId& outId) noexcept {
    std::int64_t lo = 0, hi = 0;
    if (lowIndex + 1u >= view.fieldCount ||
        !runtime::detail::loadObjectFastViewField(view, lowIndex).tryGetInt64(lo) ||
        !runtime::detail::loadObjectFastViewField(view, lowIndex + 1u).tryGetInt64(hi) ||
        lo < 0 || hi < 0 ||
        lo > std::numeric_limits<std::uint32_t>::max() ||
        hi > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    outId = static_cast<std::uint64_t>(static_cast<std::uint32_t>(lo)) |
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32u);
    return true;
}

[[nodiscard]] bool jitValidateDelegateUserArg(
    JitRuntimeSession& session,
    const runtime::Value& arg,
    semantic::PrimitiveType pt,
    semantic::SymbolId ptId,
    std::uint8_t expTag) noexcept {
    const auto vt = arg.rawTag();
    if (expTag != 0u) {
        return static_cast<std::uint8_t>(vt) == expTag;
    }
    if (pt == semantic::PrimitiveType::Object) {
        if (vt == runtime::ValueTag::NullObject) return true;
        if (vt != runtime::ValueTag::ObjectRef) return false;
        const auto ref = arg.u_.objectRef;
        if (ref.isStackSlotRef()) {
            return ref.generation == session.flatBankId;
        }
        runtime::ManagedHeap* targetHeap = nullptr;
        runtime::ErrorCode ec = runtime::ErrorCode::None;
        const char* msg = nullptr;
        if (!jitCheckCrossHeapRef(session, ref, targetHeap, ec, msg)) return false;
        const auto actTid = targetHeap->objectTypeId(ref);
        return actTid.has_value() && jitRuntimeAssignable(session, *actTid, ptId);
    }
    if (pt == semantic::PrimitiveType::String) {
        if (vt == runtime::ValueTag::NullString) return true;
        return vt == runtime::ValueTag::ObjectRef &&
               arg.u_.objectRef.kind == runtime::ObjectKind::String;
    }
    if (pt == semantic::PrimitiveType::Array) {
        if (vt == runtime::ValueTag::NullArray) return true;
        if (vt != runtime::ValueTag::ObjectRef) return false;
        runtime::ManagedHeap* targetHeap = nullptr;
        runtime::ErrorCode ec = runtime::ErrorCode::None;
        const char* msg = nullptr;
        if (!jitCheckCrossHeapRef(session, arg.u_.objectRef, targetHeap, ec, msg)) return false;
        const auto actTid = targetHeap->arrayTypeId(arg.u_.objectRef);
        return actTid.has_value() && (ptId == 0 || *actTid == ptId);
    }
    if (pt == semantic::PrimitiveType::Struct) {
        return vt == runtime::ValueTag::Struct &&
               (ptId == 0 || runtime::structValueTypeId(arg) == ptId);
    }
    if (pt == semantic::PrimitiveType::Enum) {
        runtime::EnumValue ev{};
        return arg.tryGetEnum(ev) && (ptId == 0 || ev.typeId == 0 || ev.typeId == ptId);
    }
    return true;
}

[[nodiscard]] bool jitFlattenDelegateLeaves(
    JitRuntimeSession& session,
    semantic::SymbolId delegateTypeId,
    const runtime::Value& root,
    std::vector<runtime::Value>& leaves,
    runtime::ErrorCode& errCode,
    const char*& errMsg) {
    if (root.rawTag() == runtime::ValueTag::NullObject) return true;
    if (!session.heap) {
        errCode = runtime::ErrorCode::InvalidProgram;
        errMsg = "delegate invocation has no heap";
        return false;
    }
    std::array<runtime::Value, 64> inlineStack{};
    std::size_t inlineTop = 0;
    std::vector<runtime::Value> heapStack;
    inlineStack[inlineTop++] = root;
    std::size_t visited = 0;
    while (inlineTop > 0 || !heapStack.empty()) {
        runtime::Value cur;
        if (!heapStack.empty()) {
            cur = heapStack.back();
            heapStack.pop_back();
        } else {
            cur = inlineStack[--inlineTop];
        }
        if (cur.rawTag() == runtime::ValueTag::NullObject) continue;
        if (++visited > 65536u) {
            errCode = runtime::ErrorCode::InvalidProgram;
            errMsg = "delegate invocation list is too deep";
            return false;
        }
        if (cur.rawTag() != runtime::ValueTag::ObjectRef) {
            errCode = runtime::ErrorCode::TypeMismatch;
            errMsg = "delegate invocation list contains an incompatible value";
            return false;
        }
        const auto ref = cur.u_.objectRef;
        runtime::ManagedHeap* curHeap = nullptr;
        if (!jitCheckCrossHeapRef(session, ref, curHeap, errCode, errMsg)) return false;
        runtime::ObjectFastView view{};
        if (!curHeap->tryBindObjectFastView(ref, view) ||
            view.typeId != delegateTypeId ||
            view.fieldCount != 8u) {
            errCode = runtime::ErrorCode::TypeMismatch;
            errMsg = "delegate invocation list contains an incompatible value";
            return false;
        }
        std::int64_t flags = -1;
        if (!runtime::detail::loadObjectFastViewField(view, 3).tryGetInt64(flags) ||
            !jitIsValidDelegateFlags(flags, true)) {
            errCode = runtime::ErrorCode::InvalidProgram;
            errMsg = "delegate invocation-list metadata is corrupt";
            return false;
        }
        if (flags != 4) {
            leaves.push_back(cur);
            continue;
        }
        const auto left = runtime::detail::loadObjectFastViewField(view, 0);
        const auto right = runtime::detail::loadObjectFastViewField(view, 7);
        if (!heapStack.empty() || inlineTop + 2u > inlineStack.size()) {
            heapStack.push_back(right);
            heapStack.push_back(left);
        } else {
            inlineStack[inlineTop++] = right;
            inlineStack[inlineTop++] = left;
        }
    }
    return true;
}

[[nodiscard]] bool jitSameDelegateLeaf(
    JitRuntimeSession& session,
    const runtime::Value& left,
    const runtime::Value& right) noexcept {
    if (left.rawTag() != runtime::ValueTag::ObjectRef ||
        right.rawTag() != runtime::ValueTag::ObjectRef || !session.heap) {
        return false;
    }
    const auto lRef = left.u_.objectRef;
    const auto rRef = right.u_.objectRef;
    auto* lHeap = session.executionPolicy ? session.executionPolicy->resolveHeap(lRef, session.heap) : session.heap;
    auto* rHeap = session.executionPolicy ? session.executionPolicy->resolveHeap(rRef, session.heap) : session.heap;
    if (!lHeap || !rHeap) return false;
    if (lRef == rRef && lHeap->isAlive(lRef)) return true;
    runtime::ObjectFastView lv{}, rv{};
    if (!lHeap->tryBindObjectFastView(lRef, lv) || !rHeap->tryBindObjectFastView(rRef, rv) ||
        lv.fieldCount < 7u || rv.fieldCount < 7u) {
        return false;
    }
    for (std::size_t i = 0; i < 7u; ++i) {
        if (!(runtime::detail::loadObjectFastViewField(lv, i) ==
              runtime::detail::loadObjectFastViewField(rv, i))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool jitAllocateCombineDelegateNode(
    JitAbiContext* ctx,
    std::uint32_t flatIdx,
    semantic::SymbolId delegateTypeId,
    const runtime::Value& left,
    const runtime::Value& right,
    runtime::Value& outNode,
    bool& outGcOrMoved,
    runtime::ErrorCode& errCode,
    std::string& errMsg) {
    auto& session = *ctx->session;
    const std::array<runtime::Value, 8> fields{{
        left, std::int64_t{0}, std::int64_t{0}, std::int64_t{4},
        std::int64_t{0}, std::int64_t{0}, std::int64_t{0}, right,
    }};
    const JitSpanRootGuard fieldsRoot(session.shadowStack, fields.data(), fields.size());
    const runtime::ScopedAllocationSite allocSite(makeJitAllocSite(*ctx, flatIdx));
    runtime::RuntimeError allocErr;
    bool moved = false;
    auto alloc = session.heap->allocateObjectFromTemplate(
        delegateTypeId, fields.data(), fields.size(), jitDelegateRefFields(), &allocErr, &moved);
    if (!alloc && (allocErr.code == runtime::ErrorCode::None || allocErr.code == runtime::ErrorCode::OutOfMemory)) {
        outGcOrMoved = true;
        allocErr = {};
        (void)session.heap->collectYoung(session.shadowStack);
        alloc = session.heap->allocateObjectFromTemplate(
            delegateTypeId, fields.data(), fields.size(), jitDelegateRefFields(), &allocErr, &moved);
        if (!alloc && (allocErr.code == runtime::ErrorCode::None || allocErr.code == runtime::ErrorCode::OutOfMemory)) {
            allocErr = {};
            session.heap->collectFull(session.shadowStack);
            alloc = session.heap->allocateObjectFromTemplate(
                delegateTypeId, fields.data(), fields.size(), jitDelegateRefFields(), &allocErr, &moved);
        }
    }
    if (moved) outGcOrMoved = true;
    if (!alloc) {
        errCode = allocErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::OutOfMemory : allocErr.code;
        errMsg = allocErr.message.empty() ? "delegate-list allocation failed" : allocErr.message;
        return false;
    }
    outNode = *alloc;
    return true;
}

[[nodiscard]] bool jitCombineOrRemoveDelegate(
    JitAbiContext* ctx,
    std::uint32_t flatIdx,
    semantic::SymbolId delegateTypeId,
    const runtime::Value& left,
    const runtime::Value& right,
    bool isRemove,
    runtime::Value& result,
    bool& outGcOrMoved,
    runtime::ErrorCode& errCode,
    std::string& errMsg) {
    auto& session = *ctx->session;
    if (!session.heap) {
        errCode = runtime::ErrorCode::InvalidProgram;
        errMsg = "delegate combination has no heap";
        return false;
    }
    const auto checkOperand = [&](const runtime::Value& v, std::int64_t& outFlags, runtime::ObjectFastView& outView) -> bool {
        if (v.rawTag() == runtime::ValueTag::NullObject) return true;
        if (v.rawTag() != runtime::ValueTag::ObjectRef) {
            errCode = runtime::ErrorCode::TypeMismatch;
            errMsg = "delegate combination operand has incompatible type";
            return false;
        }
        runtime::ManagedHeap* opHeap = nullptr;
        const char* cMsg = nullptr;
        if (!jitCheckCrossHeapRef(session, v.u_.objectRef, opHeap, errCode, cMsg)) {
            errMsg = cMsg;
            return false;
        }
        if (!opHeap->tryBindObjectFastView(v.u_.objectRef, outView) ||
            outView.typeId != delegateTypeId || outView.fieldCount != 8u) {
            errCode = runtime::ErrorCode::TypeMismatch;
            errMsg = "delegate combination operand has incompatible type";
            return false;
        }
        if (!runtime::detail::loadObjectFastViewField(outView, 3).tryGetInt64(outFlags) ||
            !jitIsValidDelegateFlags(outFlags, true)) {
            errCode = runtime::ErrorCode::InvalidProgram;
            errMsg = "delegate invocation-list metadata is corrupt";
            return false;
        }
        return true;
    };
    std::int64_t leftFlags = -1, rightFlags = -1;
    runtime::ObjectFastView leftView{}, rightView{};
    if (!checkOperand(left, leftFlags, leftView) || !checkOperand(right, rightFlags, rightView)) {
        return false;
    }
    const bool leftNull = (left.rawTag() == runtime::ValueTag::NullObject);
    const bool rightNull = (right.rawTag() == runtime::ValueTag::NullObject);
    const char* flatMsg = nullptr;
    if (!isRemove) {
        if (leftNull && rightNull) { result = runtime::NullObject{}; return true; }
        if (leftNull) {
            if (rightFlags == 4) {
                std::vector<runtime::Value> tmp;
                if (!jitFlattenDelegateLeaves(session, delegateTypeId, right, tmp, errCode, flatMsg)) {
                    errMsg = flatMsg; return false;
                }
            }
            result = right; return true;
        }
        if (rightNull) {
            if (leftFlags == 4) {
                std::vector<runtime::Value> tmp;
                if (!jitFlattenDelegateLeaves(session, delegateTypeId, left, tmp, errCode, flatMsg)) {
                    errMsg = flatMsg; return false;
                }
            }
            result = left; return true;
        }
        if (leftFlags == 4) {
            std::int64_t cf = -1;
            runtime::ObjectFastView cv{};
            if (!checkOperand(runtime::detail::loadObjectFastViewField(leftView, 0), cf, cv) ||
                !checkOperand(runtime::detail::loadObjectFastViewField(leftView, 7), cf, cv)) {
                return false;
            }
        }
        if (rightFlags != 4) {
            return jitAllocateCombineDelegateNode(
                ctx, flatIdx, delegateTypeId, left, right, result, outGcOrMoved, errCode, errMsg);
        }
        std::vector<runtime::Value> rightLeaves;
        if (!jitFlattenDelegateLeaves(session, delegateTypeId, right, rightLeaves, errCode, flatMsg)) {
            errMsg = flatMsg; return false;
        }
        const JitSpanRootGuard rGuard(session.shadowStack, rightLeaves.data(), rightLeaves.size());
        runtime::Value cur = left;
        for (const auto& rLeaf : rightLeaves) {
            runtime::Value nextNode;
            if (!jitAllocateCombineDelegateNode(
                    ctx, flatIdx, delegateTypeId, cur, rLeaf, nextNode, outGcOrMoved, errCode, errMsg)) {
                return false;
            }
            cur = nextNode;
        }
        result = cur;
        return true;
    }

    if (leftNull) { result = runtime::NullObject{}; return true; }
    if (rightNull) {
        if (leftFlags == 4) {
            std::vector<runtime::Value> tmp;
            if (!jitFlattenDelegateLeaves(session, delegateTypeId, left, tmp, errCode, flatMsg)) {
                errMsg = flatMsg; return false;
            }
        }
        result = left; return true;
    }
    if (leftFlags != 4 && rightFlags != 4) {
        result = jitSameDelegateLeaf(session, left, right) ? runtime::Value{runtime::NullObject{}} : left;
        return true;
    }
    std::vector<runtime::Value> leftLeaves, rightLeaves;
    if (!jitFlattenDelegateLeaves(session, delegateTypeId, left, leftLeaves, errCode, flatMsg) ||
        !jitFlattenDelegateLeaves(session, delegateTypeId, right, rightLeaves, errCode, flatMsg)) {
        errMsg = flatMsg; return false;
    }
    if (!rightLeaves.empty() && rightLeaves.size() <= leftLeaves.size()) {
        std::optional<std::size_t> removeAt;
        for (std::size_t s = 0; s + rightLeaves.size() <= leftLeaves.size(); ++s) {
            bool matches = true;
            for (std::size_t k = 0; k < rightLeaves.size(); ++k) {
                if (!jitSameDelegateLeaf(session, leftLeaves[s + k], rightLeaves[k])) {
                    matches = false; break;
                }
            }
            if (matches) removeAt = s;
        }
        if (!removeAt) { result = left; return true; }
        leftLeaves.erase(
            leftLeaves.begin() + static_cast<std::ptrdiff_t>(*removeAt),
            leftLeaves.begin() + static_cast<std::ptrdiff_t>(*removeAt + rightLeaves.size()));
    } else {
        result = left; return true;
    }
    if (leftLeaves.empty()) { result = runtime::NullObject{}; return true; }
    const JitSpanRootGuard lGuard(session.shadowStack, leftLeaves.data(), leftLeaves.size());
    runtime::Value cur = leftLeaves.front();
    for (std::size_t i = 1; i < leftLeaves.size(); ++i) {
        runtime::Value nextNode;
        if (!jitAllocateCombineDelegateNode(
                ctx, flatIdx, delegateTypeId, cur, leftLeaves[i], nextNode, outGcOrMoved, errCode, errMsg)) {
            return false;
        }
        cur = nextNode;
    }
    result = cur;
    return true;
}

} // namespace

// Fast C++ runtime helper for FlatStruct slice & StackSlotRef opcodes in StencilJit
std::uint32_t jitFlatStructAndStackRefHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    auto& session = *ctx->session;
    using FO = runtime::detail::FlatOpcode;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;
    if (ctx->remainingBudget < opDoneCost) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    const auto failOp = [&](runtime::ErrorCode code, std::string msg) noexcept -> std::uint32_t {
        if (ctx->remainingBudget >= opDoneCost) {
            ctx->remainingBudget -= opDoneCost;
        } else {
            ctx->remainingBudget = 0;
        }
        return setHelperError(ctx, flatIdx, code, std::move(msg));
    };
    const auto requireFullSpanBudget = [&]() noexcept -> bool {
        if (ctx->remainingBudget < fi.span) {
            ctx->remainingBudget = 0;
            return false;
        }
        return true;
    };

    if (fi.flatOpcode == FO::CheckNotNullFast) {
        const runtime::Value& val = ctx->frameBase[fi.src0Slot];
        if (val.rawTag() == runtime::ValueTag::ObjectRef) {
            const runtime::ObjectRef ref = val.u_.objectRef;
            bool alive = false;
            if (ref.isStackSlotRef()) {
                alive = (ref.generation == session.flatBankId);
            } else {
                runtime::ManagedHeap* targetHeap = session.executionPolicy
                    ? session.executionPolicy->resolveHeap(ref, session.heap)
                    : session.heap;
                alive = (targetHeap != nullptr && targetHeap->isAlive(ref));
            }
            if (alive) {
                if (!requireFullSpanBudget()) {
                    return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                }
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = val;
                return 0u;
            }
        }
        return failOp(runtime::ErrorCode::NullReference, "null object reference");
    }
    if (fi.flatOpcode == FO::IsTypeObjFast || fi.flatOpcode == FO::AsTypeObjFast) {
        const runtime::Value& srcVal = ctx->frameBase[fi.src0Slot];
        bool matches = false;
        if (srcVal.rawTag() == runtime::ValueTag::ObjectRef) {
            const auto ref = srcVal.u_.objectRef;
            runtime::ManagedHeap* targetHeap = nullptr;
            runtime::ErrorCode ec = runtime::ErrorCode::None;
            const char* em = nullptr;
            if (!jitCheckCrossHeapRef(session, ref, targetHeap, ec, em)) {
                return failOp(ec, em);
            }
            if (const auto actual = targetHeap->objectTypeId(ref)) {
                matches = jitRuntimeAssignable(
                    session, *actual, static_cast<semantic::SymbolId>(fi.integerImmediate));
            }
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        if (fi.flatOpcode == FO::IsTypeObjFast) {
            ctx->frameBase[fi.dstSlot].storeBool(matches);
        } else {
            ctx->frameBase[fi.dstSlot] = matches ? srcVal : runtime::Value{runtime::NullObject{}};
        }
        return 0u;
    }
    if (fi.flatOpcode == FO::NewDelegateFast) {
        if (!session.heap || !session.program) {
            return failOp(runtime::ErrorCode::InvalidProgram, "delegate allocation requires heap");
        }
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.auxImm32 >= mirror.newDelegateSites.size() || !mirror.newDelegateSites[fi.auxImm32].valid) {
            return failOp(runtime::ErrorCode::InvalidProgram, "invalid NewDelegateFast site");
        }
        const auto& site = mirror.newDelegateSites[fi.auxImm32];
        std::array<runtime::Value, 8> fields = site.templateFields;
        if (site.hasReceiverOperand) {
            if (site.expectedReceiverType != semantic::PrimitiveType::Object) {
                return failOp(runtime::ErrorCode::InvalidProgram, "unsupported delegate receiver type");
            }
            const runtime::Value& recvVal = ctx->frameBase[fi.src0Slot];
            if (recvVal.rawTag() == runtime::ValueTag::ObjectRef) {
                runtime::ManagedHeap* rHeap = nullptr;
                runtime::ErrorCode ec = runtime::ErrorCode::None;
                const char* em = nullptr;
                if (!jitCheckCrossHeapRef(session, recvVal.u_.objectRef, rHeap, ec, em)) {
                    return failOp(ec, em);
                }
                const auto actTid = rHeap->objectTypeId(recvVal.u_.objectRef);
                if (!actTid) {
                    return failOp(runtime::ErrorCode::InvalidObjectReference, "delegate receiver reference is dangling");
                }
                if (site.expectedReceiverTypeId != 0 &&
                    !jitRuntimeAssignable(session, *actTid, site.expectedReceiverTypeId)) {
                    return failOp(runtime::ErrorCode::TypeMismatch, "delegate receiver type mismatch");
                }
            } else if (recvVal.rawTag() != runtime::ValueTag::NullObject) {
                return failOp(runtime::ErrorCode::TypeMismatch, "delegate receiver type mismatch");
            }
            fields[0] = recvVal;
        }
        const JitSpanRootGuard recvGuard(session.shadowStack, fields.data(), 1u);
        const runtime::ScopedAllocationSite allocSite(makeJitAllocSite(*ctx, flatIdx));
        runtime::RuntimeError allocErr;
        bool moved = false, ranGc = false;
        const auto tryAlloc = [&]() {
            bool m = false;
            auto res = session.heap->allocateObjectFromTemplate(
                site.delegateTypeId, fields.data(), fields.size(), jitDelegateRefFields(), &allocErr, &m);
            moved = moved || m;
            return res;
        };
        auto alloc = tryAlloc();
        if (!alloc && (allocErr.code == runtime::ErrorCode::None || allocErr.code == runtime::ErrorCode::OutOfMemory)) {
            ranGc = true;
            allocErr = {};
            (void)session.heap->collectYoung(session.shadowStack);
            alloc = tryAlloc();
            if (!alloc && (allocErr.code == runtime::ErrorCode::None || allocErr.code == runtime::ErrorCode::OutOfMemory)) {
                allocErr = {};
                session.heap->collectFull(session.shadowStack);
                alloc = tryAlloc();
            }
        }
        if (!alloc) {
            if (ranGc || moved) invalidateJitArrayCaches(*ctx);
            return failOp(
                allocErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::OutOfMemory : allocErr.code,
                allocErr.message.empty() ? "delegate allocation failed" : allocErr.message);
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = *alloc;
        invalidateJitArrayCaches(*ctx);
        stepJitGcSafepoint(ctx);
        return 0u;
    }
    if (fi.flatOpcode == FO::CombineDelegateFast || fi.flatOpcode == FO::RemoveDelegateFast) {
        const std::array<runtime::Value, 2> pair{{ctx->frameBase[fi.src0Slot], ctx->frameBase[fi.src1Slot]}};
        const JitSpanRootGuard opGuard(session.shadowStack, pair.data(), pair.size());
        runtime::Value outVal;
        bool gcOrMoved = false;
        runtime::ErrorCode ec = runtime::ErrorCode::None;
        std::string em;
        if (!jitCombineOrRemoveDelegate(
                ctx, flatIdx, static_cast<semantic::SymbolId>(fi.integerImmediate),
                pair[0], pair[1], fi.flatOpcode == FO::RemoveDelegateFast,
                outVal, gcOrMoved, ec, em)) {
            if (gcOrMoved) invalidateJitArrayCaches(*ctx);
            return failOp(ec, std::move(em));
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = std::move(outVal);
        if (gcOrMoved) invalidateJitArrayCaches(*ctx);
        stepJitGcSafepoint(ctx);
        return 0u;
    }
    if (fi.flatOpcode == FO::InvokeDelegateFast) {
        if (!session.directInvokeFn || !session.moduleImpl || !session.program || !session.heap) {
            return failOp(runtime::ErrorCode::InvalidProgram, "delegate invocation environment invalid");
        }
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.auxImm32 >= mirror.invokeDelegateSites.size() || !mirror.invokeDelegateSites[fi.auxImm32].valid) {
            return failOp(runtime::ErrorCode::InvalidProgram, "invalid InvokeDelegateFast site");
        }
        const auto& siteDesc = mirror.invokeDelegateSites[fi.auxImm32];
        const runtime::Value& delVal = ctx->frameBase[fi.src0Slot];
        if (delVal.rawTag() == runtime::ValueTag::NullObject || delVal.rawTag() == runtime::ValueTag::Monostate) {
            return failOp(runtime::ErrorCode::NullReference, "delegate invocation attempted to dereference null");
        }
        if (delVal.rawTag() != runtime::ValueTag::ObjectRef) {
            return failOp(runtime::ErrorCode::TypeMismatch, "delegate invocation target is not a delegate");
        }
        runtime::ManagedHeap* delHeap = nullptr;
        runtime::ErrorCode ec = runtime::ErrorCode::None;
        const char* em = nullptr;
        if (!jitCheckCrossHeapRef(session, delVal.u_.objectRef, delHeap, ec, em)) {
            return failOp(ec, em);
        }
        runtime::ObjectFastView delView{};
        if (!delHeap->tryBindObjectFastView(delVal.u_.objectRef, delView) ||
            delView.typeId != siteDesc.delegateTypeId || delView.fieldCount != 8u) {
            return failOp(runtime::ErrorCode::TypeMismatch, "delegate invocation value has incompatible type");
        }
        std::int64_t rootFlags = -1;
        if (!runtime::detail::loadObjectFastViewField(delView, 3).tryGetInt64(rootFlags) ||
            !jitIsValidDelegateFlags(rootFlags, true)) {
            return failOp(runtime::ErrorCode::InvalidProgram, "delegate invocation-list metadata is corrupt");
        }
        const std::size_t uCount = siteDesc.userParamTypes.size();
        std::array<runtime::Value, 8> userArgsStack{};
        std::vector<runtime::Value> userArgsHeap;
        runtime::Value* userArgsPtr = userArgsStack.data();
        if (uCount > userArgsStack.size()) {
            userArgsHeap.resize(uCount);
            userArgsPtr = userArgsHeap.data();
        }
        if (fi.inlineArgCount >= 1u) {
            if (uCount >= 1u) userArgsPtr[0] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src1Slot]);
            if (uCount >= 2u) userArgsPtr[1] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src2Slot]);
        } else {
            const auto cold = execFunc.resolveCold(flatIdx);
            if (!cold.bytecode || cold.bytecode->operands.size() < uCount + 1u) {
                return failOp(runtime::ErrorCode::InvalidProgram, "InvokeDelegateFast operand metadata missing");
            }
            for (std::size_t k = 0; k < uCount; ++k) {
                userArgsPtr[k] = runtime::detachFrameSliceValue(
                    ctx->frameBase[execFunc.registerOffset + cold.bytecode->operands[k + 1u]]);
            }
        }
        for (std::size_t k = 0; k < uCount; ++k) {
            if (!jitValidateDelegateUserArg(
                    session, userArgsPtr[k], siteDesc.userParamTypes[k],
                    siteDesc.userParamTypeIds[k], siteDesc.userParamExactTags[k])) {
                return failOp(runtime::ErrorCode::TypeMismatch, "delegate argument type mismatch");
            }
        }
        runtime::Value singleLeaf = delVal;
        std::vector<runtime::Value> mcLeaves;
        const runtime::Value* leavesPtr = &singleLeaf;
        std::size_t leafCount = 1u;
        if (rootFlags == 4) {
            if (!jitFlattenDelegateLeaves(session, siteDesc.delegateTypeId, delVal, mcLeaves, ec, em)) {
                return failOp(ec, em);
            }
            leavesPtr = mcLeaves.data();
            leafCount = mcLeaves.size();
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        const JitSpanRootGuard leavesGuard(session.shadowStack, leavesPtr, leafCount);
        const JitSpanRootGuard uArgsGuard(session.shadowStack, userArgsPtr, uCount);
        runtime::Value lastResult{};
        std::array<runtime::Value, 9> callArgsStack{};
        std::vector<runtime::Value> callArgsHeap;
        for (std::size_t li = 0; li < leafCount; ++li) {
            const auto leafRef = leavesPtr[li].u_.objectRef;
            runtime::ManagedHeap* leafHeap = nullptr;
            if (!jitCheckCrossHeapRef(session, leafRef, leafHeap, ec, em)) return failOp(ec, em);
            runtime::ObjectFastView lv{};
            std::int64_t lFlags = -1, lSlot = -1;
            semantic::SymbolId lSym = 0;
            if (!leafHeap->tryBindObjectFastView(leafRef, lv) || lv.fieldCount != 8u ||
                !runtime::detail::loadObjectFastViewField(lv, 3).tryGetInt64(lFlags) ||
                !runtime::detail::loadObjectFastViewField(lv, 4).tryGetInt64(lSlot) ||
                !jitIsValidDelegateFlags(lFlags, false) ||
                lSlot < 0 || lSlot > std::numeric_limits<std::uint32_t>::max() ||
                !jitReadDelegateIdFromView(lv, 1u, lSym)) {
                return failOp(runtime::ErrorCode::InvalidProgram, "delegate invocation leaf metadata is corrupt");
            }
            const auto recvVal = runtime::detail::loadObjectFastViewField(lv, 0);
            const bool hasRecv = (recvVal.rawTag() != runtime::ValueTag::NullObject);
            if (lFlags != 0) {
                if (!hasRecv || recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
                    return failOp(runtime::ErrorCode::InvalidObjectReference, "virtual delegate receiver is invalid");
                }
                runtime::ManagedHeap* rHeap = nullptr;
                if (!jitCheckCrossHeapRef(session, recvVal.u_.objectRef, rHeap, ec, em)) return failOp(ec, em);
                const auto rTid = rHeap->objectTypeId(recvVal.u_.objectRef);
                if (!rTid) return failOp(runtime::ErrorCode::InvalidObjectReference, "virtual delegate receiver dangling");
                runtime::detail::PolymorphicCallSiteDesc pd{};
                pd.slot = static_cast<std::uint32_t>(lSlot);
                pd.isInterface = (lFlags == 2);
                if (pd.isInterface && !jitReadDelegateIdFromView(lv, 5u, pd.interfaceTypeId)) {
                    return failOp(runtime::ErrorCode::InvalidProgram, "delegate interface metadata is corrupt");
                }
                pd.valid = true;
                const auto* pre = resolveJitPolymorphicTarget(mirror, *rTid, pd);
                if (!pre || !pre->target || !pre->target->function) {
                    return failOp(runtime::ErrorCode::FunctionNotFound, "virtual delegate target not found");
                }
                lSym = pre->target->function->symbolId;
            }
            const runtime::Value* callPtr = userArgsPtr;
            const std::uint32_t callCnt = static_cast<std::uint32_t>(uCount + (hasRecv ? 1u : 0u));
            if (hasRecv) {
                runtime::Value* buf = callArgsStack.data();
                if (callCnt > callArgsStack.size()) {
                    callArgsHeap.resize(callCnt);
                    buf = callArgsHeap.data();
                }
                buf[0] = recvVal;
                for (std::size_t k = 0; k < uCount; ++k) buf[k + 1u] = userArgsPtr[k];
                callPtr = buf;
            }
            const std::uint32_t subStatus = session.directInvokeFn(
                session.moduleImpl, ctx, lSym, callPtr, callCnt, &lastResult, session.forceSimulateArm64);
            invalidateJitArrayCaches(*ctx);
            if (subStatus != 0u) {
                appendJitCallerErrorFrame(session, *ctx->compiledFn->location, *ctx->compiledFn->execFunc, flatIdx);
                ctx->errorCodeRaw = subStatus;
                ctx->errorFlatIndex = flatIdx;
                return subStatus;
            }
        }
        if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
            storeJitFlatFrameSlot(
                execFunc, ctx->frameBase, session.flatBankId, session.frameOffset,
                fi.dstSlot, std::move(lastResult));
        }
        return 0u;
    }
    if (fi.flatOpcode == FO::NewObjectFast) {
        if (!session.heap || !session.program) {
            return failOp(runtime::ErrorCode::InvalidProgram, "object allocation requires heap");
        }
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.aux0Slot >= mirror.objectTemplates.size()) {
            return failOp(runtime::ErrorCode::InvalidProgram, "invalid object template");
        }
        const auto& tmpl = mirror.objectTemplates[fi.aux0Slot];
        const runtime::ScopedAllocationSite allocSite(makeJitAllocSite(*ctx, flatIdx));
        runtime::RuntimeError allocErr;
        bool slotsMoved = false;
        bool ranGc = false;
        const auto allocObj = [&]() {
            bool attemptMoved = false;
            auto res = session.heap->allocateObjectFromTemplate(
                tmpl.typeId,
                tmpl.defaultFields.data(),
                tmpl.defaultFields.size(),
                tmpl.referenceFields,
                &allocErr,
                &attemptMoved,
                tmpl.hybridLayout,
                tmpl.defaultHybridBytes.empty() ? nullptr : tmpl.defaultHybridBytes.data());
            slotsMoved = slotsMoved || attemptMoved;
            return res;
        };
        auto allocated = allocObj();
        if (!allocated &&
            (allocErr.code == runtime::ErrorCode::None ||
             allocErr.code == runtime::ErrorCode::OutOfMemory)) {
            ranGc = true;
            allocErr = {};
            (void)session.heap->collectYoung(session.shadowStack);
            allocated = allocObj();
            if (!allocated &&
                (allocErr.code == runtime::ErrorCode::None ||
                 allocErr.code == runtime::ErrorCode::OutOfMemory)) {
                allocErr = {};
                session.heap->collectFull(session.shadowStack);
                allocated = allocObj();
            }
        }
        if (!allocated) {
            if (ranGc || slotsMoved) {
                invalidateJitArrayCaches(*ctx);
            }
            return failOp(
                allocErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::OutOfMemory : allocErr.code,
                allocErr.message.empty() ? "managed object allocation failed" : allocErr.message);
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = *allocated;
        invalidateJitArrayCaches(*ctx);
        stepJitGcSafepoint(ctx);
        return 0u;
    }
    if (fi.flatOpcode == FO::LoadFieldExactFast || fi.flatOpcode == FO::LoadFieldFast) {
        const runtime::Value& recvVal = ctx->frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            const auto errCode = (recvVal.rawTag() == runtime::ValueTag::NullObject)
                ? runtime::ErrorCode::NullReference
                : runtime::ErrorCode::TypeMismatch;
            return failOp(errCode, "field load attempted on null or non-object");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        if (ref.isStackSlotRef()) {
            if (ref.generation != session.flatBankId || !session.values) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "invalid stack ref");
            }
            if (ref.stackRefSchemaId() == 0u) {
                if (!requireFullSpanBudget()) {
                    return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                }
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = (*session.values)[ref.slot];
                return 0u;
            }
        }
        runtime::ManagedHeap* targetHeap = session.executionPolicy
            ? session.executionPolicy->resolveHeap(ref, session.heap)
            : session.heap;
        if (!session.heap || !targetHeap) {
            return failOp(runtime::ErrorCode::InvalidObjectReference, "field load missing heap");
        }
        const bool crossHeap = (targetHeap != session.heap && targetHeap->heapId() != session.heap->heapId());
        runtime::CrossHeapAccessPolicy crossPolicy = runtime::CrossHeapAccessPolicy::AllowReadWrite;
        if (crossHeap && session.executionPolicy != nullptr) {
            crossPolicy = session.executionPolicy->crossHeapAccessPolicy(session.heap, targetHeap);
            if (crossPolicy == runtime::CrossHeapAccessPolicy::Deny) {
                return failOp(runtime::ErrorCode::CapabilityDenied, "cross-heap field read denied");
            }
        }
        runtime::ObjectFastView view{};
        if (!targetHeap->tryBindObjectFastView(ref, view)) {
            return failOp(runtime::ErrorCode::InvalidObjectReference, "dangling object reference");
        }
        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        bool ownerOk = (expectedOwnerTid == 0) || (view.typeId == expectedOwnerTid);
        if (!ownerOk && session.program) {
            const auto* cur = session.program->resolveType(view.typeId);
            while (cur != nullptr) {
                if (cur->id == expectedOwnerTid) {
                    ownerOk = true;
                    break;
                }
                cur = (cur->baseTypeId != 0) ? session.program->resolveType(cur->baseTypeId) : nullptr;
            }
        }
        if (!ownerOk || fi.aux0Slot >= view.fieldCount) {
            return failOp(runtime::ErrorCode::TypeMismatch, "field load receiver type mismatch");
        }
        if (crossHeap) {
            runtime::Value readVal;
            runtime::RuntimeError readErr;
            if (!session.heap->readFieldWithPolicy(*targetHeap, ref, fi.aux0Slot, crossPolicy, readVal, &readErr)) {
                return failOp(
                    readErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::CrossHeapAccessViolation : readErr.code,
                    readErr.message.empty() ? "cross-heap field read failed" : readErr.message);
            }
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= fi.span;
            ctx->frameBase[fi.dstSlot] = std::move(readVal);
            return 0u;
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] =
            runtime::detail::loadObjectFastViewField(view, fi.aux0Slot);
        return 0u;
    }
    if (fi.flatOpcode == FO::StoreFieldExactFast || fi.flatOpcode == FO::StoreFieldFast) {
        const runtime::Value& recvVal = ctx->frameBase[fi.src0Slot];
        const runtime::Value& storeVal = ctx->frameBase[fi.src1Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            const auto errCode = (recvVal.rawTag() == runtime::ValueTag::NullObject)
                ? runtime::ErrorCode::NullReference
                : runtime::ErrorCode::TypeMismatch;
            return failOp(errCode, "field store attempted on null or non-object");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        if (ref.isStackSlotRef()) {
            if (ref.generation != session.flatBankId || !session.values) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "invalid stack ref");
            }
            if (ref.stackRefSchemaId() == 0u) {
                if (!requireFullSpanBudget()) {
                    return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                }
                (*session.values)[ref.slot] = runtime::detachFrameSliceValue(storeVal);
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
        }
        runtime::ManagedHeap* targetHeap = session.executionPolicy
            ? session.executionPolicy->resolveHeap(ref, session.heap)
            : session.heap;
        if (!session.heap || !targetHeap) {
            return failOp(runtime::ErrorCode::InvalidObjectReference, "field store missing heap");
        }
        const bool crossHeap = (targetHeap != session.heap && targetHeap->heapId() != session.heap->heapId());
        if (crossHeap && session.executionPolicy != nullptr &&
            session.executionPolicy->crossHeapAccessPolicy(session.heap, targetHeap) !=
                runtime::CrossHeapAccessPolicy::AllowReadWrite) {
            return failOp(
                runtime::ErrorCode::CrossHeapAccessViolation,
                "cross-heap field store rejected by execution policy");
        }
        const auto actualTid = targetHeap->objectTypeId(ref);
        if (!actualTid) {
            return failOp(runtime::ErrorCode::InvalidObjectReference, "dangling object reference");
        }
        const auto expectedOwnerTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        bool ownerOk = (expectedOwnerTid == 0) || (*actualTid == expectedOwnerTid);
        if (!ownerOk && session.program) {
            const auto* cur = session.program->resolveType(*actualTid);
            while (cur != nullptr) {
                if (cur->id == expectedOwnerTid) {
                    ownerOk = true;
                    break;
                }
                cur = (cur->baseTypeId != 0) ? session.program->resolveType(cur->baseTypeId) : nullptr;
            }
        }
        if (!ownerOk) {
            return failOp(runtime::ErrorCode::TypeMismatch, "field store receiver type mismatch");
        }
        const runtime::Value cleanVal = runtime::detachFrameSliceValue(storeVal);
        runtime::RuntimeError setErr;
        if (session.executionPolicy != nullptr) {
            semantic::SymbolId declTid = (expectedOwnerTid != 0) ? expectedOwnerTid : *actualTid;
            std::string_view fieldName;
            if (session.program) {
                const auto* declType = session.program->resolveType(declTid);
                if (!declType && declTid != *actualTid) {
                    declType = session.program->resolveType(*actualTid);
                }
                if (declType != nullptr) {
                    declTid = declType->id;
                    if (fi.aux0Slot < declType->fields.size()) {
                        fieldName = declType->fields[fi.aux0Slot].name;
                    }
                }
            }
            if (!session.executionPolicy->checkFieldWrite(
                    session.heap, targetHeap, ref, declTid, fi.aux0Slot, fieldName, cleanVal, setErr)) {
                return failOp(
                    setErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::CapabilityDenied : setErr.code,
                    setErr.message.empty() ? "field store denied by execution policy" : setErr.message);
            }
        }
        const bool wroteField = crossHeap
            ? session.heap->writeFieldWithPolicy(
                  *targetHeap,
                  ref,
                  fi.aux0Slot,
                  cleanVal,
                  runtime::CrossHeapAccessPolicy::AllowReadWrite,
                  &setErr)
            : targetHeap->fieldSet(ref, fi.aux0Slot, cleanVal, &setErr);
        if (!wroteField) {
            return failOp(
                setErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::InvalidProgram : setErr.code,
                setErr.message.empty() ? "field store failed" : setErr.message);
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    if (fi.flatOpcode == FO::GenericBytecode) {
        const auto cold = execFunc.resolveCold(flatIdx);
        if (flatIdx >= execFunc.flatCold.size() ||
            cold.bytecode == nullptr ||
            execFunc.bytecode == nullptr) {
            return failOp(runtime::ErrorCode::InvalidProgram, "missing GenericBytecode metadata");
        }
        const auto& bc = *cold.bytecode;
        const auto& bcFn = *execFunc.bytecode;
        const auto regOff = execFunc.registerOffset;
        switch (bc.opcode) {
        case bytecode::Opcode::ConstantInt: {
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            const auto resType = bcFn.registerTypes[bc.result];
            if (resType == semantic::PrimitiveType::Long) {
                ctx->frameBase[regOff + bc.result] = runtime::LongValue{bc.integerImmediate};
            } else if (resType == semantic::PrimitiveType::Enum) {
                const auto tid = bc.result < bcFn.registerTypeIds.size() ? bcFn.registerTypeIds[bc.result] : 0;
                ctx->frameBase[regOff + bc.result] = runtime::EnumValue{tid, bc.integerImmediate};
            } else {
                ctx->frameBase[regOff + bc.result] = bc.integerImmediate;
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        case bytecode::Opcode::ConstantNull: {
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            const auto resType = bcFn.registerTypes[bc.result];
            if (resType == semantic::PrimitiveType::Null) {
                ctx->frameBase[regOff + bc.result] = std::monostate{};
            } else if (resType == semantic::PrimitiveType::String) {
                ctx->frameBase[regOff + bc.result] = runtime::NullString{};
            } else if (resType == semantic::PrimitiveType::Array) {
                ctx->frameBase[regOff + bc.result] = runtime::NullArray{};
            } else {
                ctx->frameBase[regOff + bc.result] = runtime::NullObject{};
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        case bytecode::Opcode::ConvertNullToString:
        case bytecode::Opcode::ConvertNullToObject:
        case bytecode::Opcode::ConvertNullToArray: {
            const runtime::Value& src = ctx->frameBase[regOff + bc.operands[0]];
            const auto tag = src.rawTag();
            if (tag != runtime::ValueTag::Monostate &&
                tag != runtime::ValueTag::NullString &&
                tag != runtime::ValueTag::NullObject &&
                tag != runtime::ValueTag::NullArray) {
                return failOp(runtime::ErrorCode::TypeMismatch, "expected null for conversion");
            }
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            if (bc.opcode == bytecode::Opcode::ConvertNullToString) {
                ctx->frameBase[regOff + bc.result] = runtime::NullString{};
            } else if (bc.opcode == bytecode::Opcode::ConvertNullToArray) {
                ctx->frameBase[regOff + bc.result] = runtime::NullArray{};
            } else {
                ctx->frameBase[regOff + bc.result] = runtime::NullObject{};
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        case bytecode::Opcode::ConstantString: {
            if (!session.heap) {
                return failOp(runtime::ErrorCode::InvalidProgram, "ConstantString requires heap");
            }
            const runtime::ScopedAllocationSite allocSite(makeJitAllocSite(*ctx, flatIdx));
            runtime::RuntimeError strErr;
            bool ranGc = false;
            const auto interned = jitInternStringWithOomRetry(session, bc.stringImmediate, strErr, ranGc);
            if (!interned) {
                if (ranGc) {
                    invalidateJitArrayCaches(*ctx);
                }
                return failOp(
                    strErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::OutOfMemory : strErr.code,
                    strErr.message);
            }
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->frameBase[regOff + bc.result] = *interned;
            ctx->remainingBudget -= fi.span;
            invalidateJitArrayCaches(*ctx);
            stepJitGcSafepoint(ctx);
            return 0u;
        }
        case bytecode::Opcode::Equal:
        case bytecode::Opcode::NotEqual: {
            const runtime::Value& lhs = ctx->frameBase[regOff + bc.operands[0]];
            const runtime::Value& rhs = ctx->frameBase[regOff + bc.operands[1]];
            bool eq = false;
            runtime::ErrorCode eqErrCode = runtime::ErrorCode::InvalidObjectReference;
            const char* eqErrMsg = "equality comparison failed";
            if (!jitValuesEqual(session.heap, lhs, rhs, eq, eqErrCode, eqErrMsg)) {
                return failOp(eqErrCode, eqErrMsg);
            }
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->frameBase[regOff + bc.result] = (bc.opcode == bytecode::Opcode::Equal) ? eq : !eq;
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        default:
            return failOp(runtime::ErrorCode::InvalidProgram, "unsupported GenericBytecode in StencilJit");
        }
    }
    if (fi.flatOpcode == FO::ConstantNullTagFast) {
        if (fi.aux0Slot == 0u) {
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->frameBase[fi.dstSlot] = std::monostate{};
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        const auto tag = ctx->frameBase[fi.src0Slot].rawTag();
        if (tag != runtime::ValueTag::Monostate &&
            tag != runtime::ValueTag::NullString &&
            tag != runtime::ValueTag::NullObject &&
            tag != runtime::ValueTag::NullArray) {
            return failOp(runtime::ErrorCode::TypeMismatch, "expected null for conversion");
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        if (fi.aux0Slot == 1u) {
            ctx->frameBase[fi.dstSlot] = runtime::NullObject{};
        } else if (fi.aux0Slot == 2u) {
            ctx->frameBase[fi.dstSlot] = runtime::NullString{};
        } else {
            ctx->frameBase[fi.dstSlot] = runtime::NullArray{};
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    if (fi.flatOpcode == FO::ConstantEnumFast) {
        if (!session.program) {
            return failOp(runtime::ErrorCode::InvalidProgram, "ConstantEnumFast missing program");
        }
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.auxImm32 >= mirror.constPoolValues.size()) {
            return failOp(runtime::ErrorCode::InvalidProgram, "ConstantEnumFast pool index out of bounds");
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->frameBase[fi.dstSlot] = mirror.constPoolValues[fi.auxImm32];
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    if (fi.flatOpcode == FO::ConstantStringFast) {
        if (!session.heap || !session.program) {
            return failOp(runtime::ErrorCode::InvalidProgram, "ConstantStringFast requires heap and program");
        }
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.auxImm32 >= mirror.stringLiterals.size()) {
            return failOp(runtime::ErrorCode::InvalidProgram, "ConstantStringFast literal index out of bounds");
        }
        const std::uint64_t curHeapId = session.heap->heapId();
        const std::uint64_t curEpoch = session.heap->stringInternEpoch();
        const bool gcIdle =
            session.heap->phase() == runtime::GcPhase::Idle &&
            !session.heap->collectionRequested();
        if (gcIdle &&
            session.stringLiteralCache != nullptr &&
            session.stringLiteralCacheEpoch != nullptr &&
            session.stringLiteralCacheHeapId != nullptr &&
            *session.stringLiteralCacheHeapId == curHeapId &&
            *session.stringLiteralCacheEpoch == curEpoch &&
            fi.auxImm32 < session.stringLiteralCache->size()) {
            const runtime::ObjectRef cached = (*session.stringLiteralCache)[fi.auxImm32];
            if (cached.valid()) {
                if (!requireFullSpanBudget()) {
                    return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
                }
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = cached;
                return 0u;
            }
        }
        const runtime::ScopedAllocationSite allocSite(makeJitAllocSite(*ctx, flatIdx));
        runtime::RuntimeError strErr;
        bool ranGc = false;
        const auto interned = jitInternStringWithOomRetry(
            session, mirror.stringLiterals[fi.auxImm32], strErr, ranGc);
        if (!interned) {
            if (ranGc) {
                invalidateJitArrayCaches(*ctx);
            }
            return failOp(
                strErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::OutOfMemory : strErr.code,
                strErr.message.empty() ? "string literal interning failed" : strErr.message);
        }
        if (session.stringLiteralCache != nullptr &&
            session.stringLiteralCacheEpoch != nullptr &&
            session.stringLiteralCacheHeapId != nullptr) {
            if (session.stringLiteralCache->size() < mirror.stringLiterals.size()) {
                session.stringLiteralCache->resize(mirror.stringLiterals.size());
            }
            const std::uint64_t afterEpoch = session.heap->stringInternEpoch();
            if (*session.stringLiteralCacheHeapId != curHeapId ||
                *session.stringLiteralCacheEpoch != afterEpoch) {
                std::fill(
                    session.stringLiteralCache->begin(),
                    session.stringLiteralCache->end(),
                    runtime::ObjectRef{});
                *session.stringLiteralCacheHeapId = curHeapId;
                *session.stringLiteralCacheEpoch = afterEpoch;
            }
            if (fi.auxImm32 < session.stringLiteralCache->size()) {
                (*session.stringLiteralCache)[fi.auxImm32] = *interned;
            }
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = *interned;
        invalidateJitArrayCaches(*ctx);
        stepJitGcSafepoint(ctx);
        return 0u;
    }
    if (fi.flatOpcode == FO::EqualValueFast) {
        const runtime::Value& lhs = ctx->frameBase[fi.src0Slot];
        const runtime::Value& rhs = ctx->frameBase[fi.src1Slot];
        runtime::ManagedHeap* lHeap = session.heap;
        runtime::ManagedHeap* rHeap = session.heap;
        if (session.executionPolicy != nullptr) {
            if (lhs.rawTag() == runtime::ValueTag::ObjectRef && !lhs.asObjectRefUnchecked().isStackSlotRef()) {
                lHeap = session.executionPolicy->resolveHeap(lhs.asObjectRefUnchecked(), session.heap);
            }
            if (rhs.rawTag() == runtime::ValueTag::ObjectRef && !rhs.asObjectRefUnchecked().isStackSlotRef()) {
                rHeap = session.executionPolicy->resolveHeap(rhs.asObjectRefUnchecked(), session.heap);
            }
        }
        bool eq = false;
        runtime::ErrorCode eqErrCode = runtime::ErrorCode::InvalidObjectReference;
        const char* eqErrMsg = "equality comparison failed";
        if (lhs.rawTag() == runtime::ValueTag::ObjectRef &&
            rhs.rawTag() == runtime::ValueTag::ObjectRef &&
            lHeap != rHeap) {
            const auto lr = lhs.asObjectRefUnchecked();
            const auto rr = rhs.asObjectRefUnchecked();
            if (lr.kind == runtime::ObjectKind::String && rr.kind == runtime::ObjectKind::String) {
                const auto ls = lHeap ? lHeap->stringView(lr) : std::nullopt;
                const auto rs = rHeap ? rHeap->stringView(rr) : std::nullopt;
                if (!ls || !rs) {
                    return failOp(runtime::ErrorCode::InvalidObjectReference, "string equality encountered an invalid managed reference");
                }
                eq = (*ls == *rs);
            } else {
                const bool lAlive = lr.isStackSlotRef() || (lHeap != nullptr && lHeap->isAlive(lr));
                const bool rAlive = rr.isStackSlotRef() || (rHeap != nullptr && rHeap->isAlive(rr));
                if (!lAlive || !rAlive) {
                    return failOp(runtime::ErrorCode::InvalidObjectReference, "reference equality encountered an invalid managed reference");
                }
                eq = (lr == rr);
            }
        } else {
            runtime::ManagedHeap* eqHeap = session.heap;
            if (lhs.rawTag() == runtime::ValueTag::ObjectRef && !lhs.asObjectRefUnchecked().isStackSlotRef()) {
                eqHeap = lHeap;
            } else if (rhs.rawTag() == runtime::ValueTag::ObjectRef && !rhs.asObjectRefUnchecked().isStackSlotRef()) {
                eqHeap = rHeap;
            }
            if (!jitValuesEqual(eqHeap, lhs, rhs, eq, eqErrCode, eqErrMsg)) {
                return failOp(eqErrCode, eqErrMsg);
            }
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot].storeBool(fi.aux0Slot != 0u ? !eq : eq);
        return 0u;
    }
    if (fi.flatOpcode == FO::ConvertNumericFast) {
        const auto targetPt = static_cast<semantic::PrimitiveType>(
            static_cast<std::uint8_t>(fi.binaryOpcode1));
        const runtime::Value& sv = ctx->frameBase[fi.src0Slot];
        runtime::Value converted;
        bool okConvert = false;
        if (runtime::EnumValue ev{}; sv.tryGetEnum(ev)) {
            okConvert = runtime::tryConvertNumeric(
                runtime::Value{ev.value}, targetPt, converted, fi.checked1());
        } else {
            okConvert = runtime::tryConvertNumeric(sv, targetPt, converted, fi.checked1());
        }
        if (!okConvert) {
            return failOp(runtime::ErrorCode::IntegerOverflow, "numeric conversion overflow");
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = std::move(converted);
        return 0u;
    }
    if (fi.flatOpcode == FO::BinaryUIntFast) {
        const runtime::Value& lv = ctx->frameBase[fi.src0Slot];
        const runtime::Value& rv = ctx->frameBase[fi.src1Slot];
        if (lv.rawTag() != runtime::ValueTag::UInt || rv.rawTag() != runtime::ValueTag::UInt) {
            return failOp(runtime::ErrorCode::TypeMismatch, "unsigned integer operation expected uint operands");
        }
        const std::uint32_t a = lv.u_.uintCell.payload.value;
        const std::uint32_t b = rv.u_.uintCell.payload.value;
        const auto op = static_cast<bytecode::Opcode>(fi.aux0Slot);
        const bool chk = fi.checked1();
        runtime::Value out;
        switch (op) {
        case bytecode::Opcode::AddInt:
            if (chk && b > std::numeric_limits<std::uint32_t>::max() - a) {
                return failOp(runtime::ErrorCode::IntegerOverflow, "unsigned integer addition overflow");
            }
            out = runtime::UIntValue{static_cast<std::uint32_t>(a + b)};
            break;
        case bytecode::Opcode::SubtractInt:
            if (chk && a < b) {
                return failOp(runtime::ErrorCode::IntegerOverflow, "unsigned integer subtraction underflow");
            }
            out = runtime::UIntValue{static_cast<std::uint32_t>(a - b)};
            break;
        case bytecode::Opcode::MultiplyInt:
            if (chk && b != 0u && a > std::numeric_limits<std::uint32_t>::max() / b) {
                return failOp(runtime::ErrorCode::IntegerOverflow, "unsigned integer multiplication overflow");
            }
            out = runtime::UIntValue{static_cast<std::uint32_t>(a * b)};
            break;
        case bytecode::Opcode::DivideInt:
            if (b == 0u) {
                return failOp(runtime::ErrorCode::DivisionByZero, "division by zero");
            }
            out = runtime::UIntValue{static_cast<std::uint32_t>(a / b)};
            break;
        case bytecode::Opcode::RemainderInt:
            if (b == 0u) {
                return failOp(runtime::ErrorCode::DivisionByZero, "remainder by zero");
            }
            out = runtime::UIntValue{static_cast<std::uint32_t>(a % b)};
            break;
        case bytecode::Opcode::LessInt:
            out.storeBool(a < b);
            break;
        case bytecode::Opcode::LessOrEqualInt:
            out.storeBool(a <= b);
            break;
        case bytecode::Opcode::GreaterInt:
            out.storeBool(a > b);
            break;
        case bytecode::Opcode::GreaterOrEqualInt:
            out.storeBool(a >= b);
            break;
        default:
            return failOp(runtime::ErrorCode::InvalidProgram, "unsupported unsigned binary opcode");
        }
        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = std::move(out);
        return 0u;
    }
    if (fi.flatOpcode == FO::CallIntrinsicFast) {
        const auto ikind = static_cast<runtime::detail::FastIntrinsicKind>(
            static_cast<std::uint8_t>(fi.binaryOpcode1));
        switch (ikind) {
        case runtime::detail::FastIntrinsicKind::StaticHas: {
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            const auto sid = static_cast<semantic::SymbolId>(fi.integerImmediate);
            ctx->remainingBudget -= fi.span;
            ctx->frameBase[fi.dstSlot].storeBool(session.heap != nullptr && session.heap->hasStaticField(sid));
            return 0u;
        }
        case runtime::detail::FastIntrinsicKind::StaticGet: {
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            const auto sid = static_cast<semantic::SymbolId>(fi.integerImmediate);
            runtime::Value resVal{};
            if (session.heap != nullptr) {
                if (auto existing = session.heap->getStaticField(sid)) {
                    resVal = std::move(*existing);
                } else {
                    if (fi.inlineArgCount >= 1u) {
                        resVal = runtime::detachFrameSliceValue(ctx->frameBase[fi.src0Slot]);
                    }
                    session.heap->setStaticField(sid, resVal);
                }
            } else if (fi.inlineArgCount >= 1u) {
                resVal = runtime::detachFrameSliceValue(ctx->frameBase[fi.src0Slot]);
            }
            ctx->remainingBudget -= fi.span;
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                storeJitFlatFrameSlot(
                    execFunc,
                    ctx->frameBase,
                    session.flatBankId,
                    session.frameOffset,
                    fi.dstSlot,
                    std::move(resVal));
            }
            return 0u;
        }
        case runtime::detail::FastIntrinsicKind::StaticSet: {
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            if (session.heap != nullptr) {
                session.heap->setStaticField(
                    static_cast<semantic::SymbolId>(fi.integerImmediate),
                    runtime::detachFrameSliceValue(ctx->frameBase[fi.src0Slot]));
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        case runtime::detail::FastIntrinsicKind::ArrayPush: {
            const runtime::Value& arrVal = ctx->frameBase[fi.src0Slot];
            if (arrVal.rawTag() == runtime::ValueTag::NullArray) {
                return failOp(runtime::ErrorCode::NullReference, "cannot invoke array method on null array");
            }
            if (arrVal.rawTag() != runtime::ValueTag::ObjectRef) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "invalid array reference");
            }
            const runtime::ObjectRef arrRef = arrVal.u_.objectRef;
            runtime::ManagedHeap* arrHeap = session.executionPolicy
                ? session.executionPolicy->resolveHeap(arrRef, session.heap)
                : session.heap;
            if (!arrHeap) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "array push missing heap");
            }
            if (session.executionPolicy != nullptr &&
                arrHeap != session.heap &&
                arrHeap->heapId() != session.heap->heapId() &&
                session.executionPolicy->crossHeapAccessPolicy(session.heap, arrHeap) !=
                    runtime::CrossHeapAccessPolicy::AllowReadWrite) {
                return failOp(runtime::ErrorCode::CrossHeapAccessViolation, "cross-heap array push denied by execution policy");
            }
            const runtime::Value cleanElem = runtime::detachFrameSliceValue(ctx->frameBase[fi.src1Slot]);
            if (session.executionPolicy != nullptr) {
                runtime::RuntimeError policyErr;
                if (!session.executionPolicy->checkArrayWrite(session.heap, arrHeap, arrRef, cleanElem, policyErr)) {
                    return failOp(
                        policyErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::CapabilityDenied : policyErr.code,
                        policyErr.message.empty() ? "array push denied by execution policy" : policyErr.message);
                }
            }
            const JitSpanRootGuard elemRoot(session.shadowStack, &cleanElem, 1u);
            const runtime::ScopedAllocationSite allocSite(makeJitAllocSite(*ctx, flatIdx));
            runtime::RuntimeError mutErr;
            bool pushed = arrHeap->arrayPush(
                arrRef,
                cleanElem,
                &mutErr,
                runtime::ArrayStoreTypePolicy::PrevalidatedAssignableObject);
            if (!pushed && mutErr.code == runtime::ErrorCode::OutOfMemory && arrHeap == session.heap) {
                mutErr = {};
                (void)session.heap->collectYoung(session.shadowStack);
                pushed = arrHeap->arrayPush(
                    arrRef,
                    cleanElem,
                    &mutErr,
                    runtime::ArrayStoreTypePolicy::PrevalidatedAssignableObject);
                if (!pushed && mutErr.code == runtime::ErrorCode::OutOfMemory) {
                    mutErr = {};
                    session.heap->collectFull(session.shadowStack);
                    pushed = arrHeap->arrayPush(
                        arrRef,
                        cleanElem,
                        &mutErr,
                        runtime::ArrayStoreTypePolicy::PrevalidatedAssignableObject);
                }
            }
            invalidateJitArrayCaches(*ctx);
            if (!pushed) {
                return failOp(
                    mutErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::InvalidObjectReference : mutErr.code,
                    mutErr.message.empty() ? "array push failed" : mutErr.message);
            }
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= fi.span;
            stepJitGcSafepoint(ctx);
            return 0u;
        }
        case runtime::detail::FastIntrinsicKind::ArrayRemoveAt: {
            const runtime::Value& arrVal = ctx->frameBase[fi.src0Slot];
            if (arrVal.rawTag() == runtime::ValueTag::NullArray) {
                return failOp(runtime::ErrorCode::NullReference, "cannot invoke array method on null array");
            }
            if (arrVal.rawTag() != runtime::ValueTag::ObjectRef) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "invalid array reference");
            }
            const runtime::ObjectRef arrRef = arrVal.u_.objectRef;
            runtime::ManagedHeap* arrHeap = session.executionPolicy
                ? session.executionPolicy->resolveHeap(arrRef, session.heap)
                : session.heap;
            if (!arrHeap) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "array remove_at missing heap");
            }
            if (session.executionPolicy != nullptr &&
                arrHeap != session.heap &&
                arrHeap->heapId() != session.heap->heapId() &&
                session.executionPolicy->crossHeapAccessPolicy(session.heap, arrHeap) !=
                    runtime::CrossHeapAccessPolicy::AllowReadWrite) {
                return failOp(runtime::ErrorCode::CrossHeapAccessViolation, "cross-heap array remove_at denied by execution policy");
            }
            std::int64_t idx = 0;
            if (!ctx->frameBase[fi.src1Slot].tryGetInt64(idx)) {
                return failOp(runtime::ErrorCode::TypeMismatch, "array index must be integer");
            }
            if (idx < 0) {
                return failOp(runtime::ErrorCode::IndexOutOfRange, "array index out of range");
            }
            if (session.executionPolicy != nullptr) {
                runtime::RuntimeError policyErr;
                if (!session.executionPolicy->checkArrayWrite(session.heap, arrHeap, arrRef, runtime::Value{std::monostate{}}, policyErr)) {
                    return failOp(
                        policyErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::CapabilityDenied : policyErr.code,
                        policyErr.message.empty() ? "array remove_at denied by execution policy" : policyErr.message);
                }
            }
            runtime::RuntimeError mutErr;
            if (!arrHeap->arrayRemoveAt(arrRef, static_cast<std::size_t>(idx), nullptr, &mutErr)) {
                return failOp(
                    mutErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::InvalidObjectReference : mutErr.code,
                    mutErr.message.empty() ? "array remove_at failed" : mutErr.message);
            }
            invalidateJitArrayCaches(*ctx);
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                ctx->frameBase[fi.dstSlot] = std::monostate{};
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        default: {
            if (!session.heap) {
                return failOp(runtime::ErrorCode::InvalidProgram, "CallIntrinsicFast requires heap");
            }
            std::array<runtime::Value, 3> intrArgs{};
            const std::size_t nArgs = fi.inlineArgCount;
            if (nArgs > 3u) {
                return failOp(runtime::ErrorCode::InvalidProgram, "CallIntrinsicFast argument count out of range");
            }
            if (nArgs >= 1u) intrArgs[0] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src0Slot]);
            if (nArgs >= 2u) intrArgs[1] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src1Slot]);
            if (nArgs >= 3u) intrArgs[2] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src2Slot]);

            runtime::ManagedHeap* targetHeap = session.heap;
            if ((ikind == runtime::detail::FastIntrinsicKind::ArrayPop ||
                 ikind == runtime::detail::FastIntrinsicKind::ArrayClear) &&
                nArgs >= 1u &&
                intrArgs[0].rawTag() == runtime::ValueTag::ObjectRef) {
                const auto arrRef = intrArgs[0].u_.objectRef;
                if (session.executionPolicy != nullptr) {
                    targetHeap = session.executionPolicy->resolveHeap(arrRef, session.heap);
                    if (!targetHeap) {
                        return failOp(runtime::ErrorCode::InvalidObjectReference, "array mutation missing heap");
                    }
                    if (targetHeap != session.heap &&
                        targetHeap->heapId() != session.heap->heapId() &&
                        session.executionPolicy->crossHeapAccessPolicy(session.heap, targetHeap) !=
                            runtime::CrossHeapAccessPolicy::AllowReadWrite) {
                        return failOp(runtime::ErrorCode::CrossHeapAccessViolation, "cross-heap array mutation denied by execution policy");
                    }
                    runtime::RuntimeError policyErr;
                    if (!session.executionPolicy->checkArrayWrite(session.heap, targetHeap, arrRef, runtime::Value{std::monostate{}}, policyErr)) {
                        return failOp(
                            policyErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::CapabilityDenied : policyErr.code,
                            policyErr.message.empty() ? "array mutation denied by execution policy" : policyErr.message);
                    }
                }
            } else if (session.executionPolicy != nullptr) {
                for (std::size_t ai = 0; ai < nArgs; ++ai) {
                    if (intrArgs[ai].rawTag() == runtime::ValueTag::ObjectRef &&
                        intrArgs[ai].u_.objectRef.kind == runtime::ObjectKind::String) {
                        const auto strRef = intrArgs[ai].u_.objectRef;
                        if (auto* strHeap = session.executionPolicy->resolveHeap(strRef, session.heap);
                            strHeap != nullptr && strHeap != session.heap) {
                            if (const auto sv = strHeap->stringView(strRef)) {
                                intrArgs[ai] = runtime::StringValue{std::string(*sv)};
                            }
                        }
                    }
                }
            }

            const JitSpanRootGuard argsRoot(session.shadowStack, intrArgs.data(), nArgs);
            const runtime::ScopedAllocationSite allocSite(makeJitAllocSite(*ctx, flatIdx));
            runtime::Value intrOut;
            runtime::RuntimeError intrErr;
            bool ranGc = false;
            bool ok = runtime::detail::tryExecuteFastIntrinsic(
                static_cast<std::uint8_t>(ikind),
                intrArgs.data(),
                nArgs,
                targetHeap,
                intrOut,
                intrErr);
            if (!ok && intrErr.code == runtime::ErrorCode::OutOfMemory && targetHeap == session.heap) {
                ranGc = true;
                intrErr = {};
                (void)session.heap->collectYoung(session.shadowStack);
                ok = runtime::detail::tryExecuteFastIntrinsic(
                    static_cast<std::uint8_t>(ikind),
                    intrArgs.data(),
                    nArgs,
                    targetHeap,
                    intrOut,
                    intrErr);
                if (!ok && intrErr.code == runtime::ErrorCode::OutOfMemory) {
                    intrErr = {};
                    session.heap->collectFull(session.shadowStack);
                    ok = runtime::detail::tryExecuteFastIntrinsic(
                        static_cast<std::uint8_t>(ikind),
                        intrArgs.data(),
                        nArgs,
                        targetHeap,
                        intrOut,
                        intrErr);
                }
            }
            if (!ok) {
                if (ranGc) {
                    invalidateJitArrayCaches(*ctx);
                }
                return failOp(
                    intrErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::InvalidProgram : intrErr.code,
                    intrErr.message.empty() ? "fast intrinsic execution failed" : intrErr.message);
            }
            if (ranGc ||
                ikind == runtime::detail::FastIntrinsicKind::ArrayRemoveAt ||
                ikind == runtime::detail::FastIntrinsicKind::ArrayPop ||
                ikind == runtime::detail::FastIntrinsicKind::ArrayClear) {
                invalidateJitArrayCaches(*ctx);
            }
            if (!requireFullSpanBudget()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
                storeJitFlatFrameSlot(
                    execFunc,
                    ctx->frameBase,
                    session.flatBankId,
                    session.frameOffset,
                    fi.dstSlot,
                    std::move(intrOut));
            }
            ctx->remainingBudget -= fi.span;
            stepJitGcSafepoint(ctx);
            return 0u;
        }
        }
    }
    if (fi.flatOpcode == FO::CallDirect0 ||
        fi.flatOpcode == FO::CallDirect1 ||
        fi.flatOpcode == FO::CallDirect2 ||
        fi.flatOpcode == FO::CallDirectN ||
        fi.flatOpcode == FO::CallPolledFast ||
        fi.flatOpcode == FO::SuperCheckNotNullCallPolledFast) {
        if (!session.directInvokeFn || !session.moduleImpl || !session.program ||
            flatIdx >= execFunc.flatCold.size()) {
            return failOp(runtime::ErrorCode::InvalidProgram, "direct/polymorphic call metadata missing in StencilJit");
        }
        const auto cold = execFunc.resolveCold(flatIdx);
        semantic::SymbolId targetSymId = 0;
        std::array<runtime::Value, 8> stackArgs{};
        std::vector<runtime::Value> heapArgs;
        const runtime::Value* argsPtr = nullptr;
        std::uint32_t argCount = 0;

        if (fi.flatOpcode == FO::CallDirect0 ||
            fi.flatOpcode == FO::CallDirect1 ||
            fi.flatOpcode == FO::CallDirect2 ||
            fi.flatOpcode == FO::CallDirectN) {
            if (!cold.directTarget || !cold.directTarget->function) {
                return failOp(runtime::ErrorCode::InvalidProgram, "missing direct call target in StencilJit");
            }
            targetSymId = cold.directTarget->function->symbolId;
            if (fi.flatOpcode == FO::CallDirect1) {
                argCount = 1u;
                stackArgs[0] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src0Slot]);
                argsPtr = stackArgs.data();
            } else if (fi.flatOpcode == FO::CallDirect2) {
                argCount = 2u;
                stackArgs[0] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src0Slot]);
                stackArgs[1] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src1Slot]);
                argsPtr = stackArgs.data();
            } else if (fi.flatOpcode == FO::CallDirectN) {
                if (!cold.bytecode) {
                    return failOp(runtime::ErrorCode::InvalidProgram, "missing CallDirectN bytecode metadata");
                }
                const auto& ops = cold.bytecode->operands;
                argCount = static_cast<std::uint32_t>(ops.size());
                if (argCount <= stackArgs.size()) {
                    for (std::uint32_t i = 0; i < argCount; ++i) {
                        stackArgs[i] = runtime::detachFrameSliceValue(
                            ctx->frameBase[execFunc.registerOffset + ops[i]]);
                    }
                    argsPtr = stackArgs.data();
                } else {
                    heapArgs.resize(argCount);
                    for (std::uint32_t i = 0; i < argCount; ++i) {
                        heapArgs[i] = runtime::detachFrameSliceValue(
                            ctx->frameBase[execFunc.registerOffset + ops[i]]);
                    }
                    argsPtr = heapArgs.data();
                }
            }
        } else {
            const runtime::Value& recvVal = ctx->frameBase[fi.src0Slot];
            if (recvVal.rawTag() == runtime::ValueTag::NullObject) {
                return failOp(runtime::ErrorCode::NullReference, "polymorphic call on null object reference");
            }
            if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "polymorphic call on invalid object reference");
            }
            const runtime::ObjectRef recvRef = recvVal.u_.objectRef;
            runtime::ManagedHeap* recvHeap = session.executionPolicy
                ? session.executionPolicy->resolveHeap(recvRef, session.heap)
                : session.heap;
            if (!recvHeap) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "polymorphic call missing heap");
            }
            if (recvHeap != session.heap && session.executionPolicy != nullptr &&
                session.executionPolicy->crossHeapAccessPolicy(session.heap, recvHeap) == runtime::CrossHeapAccessPolicy::Deny) {
                return failOp(runtime::ErrorCode::CapabilityDenied, "cross-heap polymorphic call denied");
            }
            const auto actualTid = recvHeap->objectTypeId(recvRef);
            if (!actualTid) {
                return failOp(runtime::ErrorCode::InvalidObjectReference, "polymorphic call on dangling object reference");
            }
            const auto cacheIdx =
                (fi.auxImm32 ^ static_cast<std::uint32_t>(*actualTid >> 4u)) & 15u;
            auto& pce = session.polyCallCache[cacheIdx];
            if (pce.siteIdx == fi.auxImm32 &&
                pce.receiverTypeId == *actualTid &&
                pce.targetSymbolId != 0u) {
                targetSymId = pce.targetSymbolId;
            } else {
                const auto& mirror = runtime::detail::executionMirror(*session.program);
                if (fi.auxImm32 >= mirror.polyCallSites.size()) {
                    return failOp(runtime::ErrorCode::InvalidProgram, "polymorphic call site index out of range");
                }
                const auto* pre = resolveJitPolymorphicTarget(
                    mirror, *actualTid, mirror.polyCallSites[fi.auxImm32]);
                if (!pre || !pre->target || !pre->target->function) {
                    return failOp(runtime::ErrorCode::FunctionNotFound, "polymorphic method target not found");
                }
                targetSymId = pre->target->function->symbolId;
                pce.siteIdx = fi.auxImm32;
                pce.receiverTypeId = *actualTid;
                pce.targetSymbolId = targetSymId;
            }
            if (fi.inlineArgCount >= 1u && fi.inlineArgCount <= 3u) {
                argCount = fi.inlineArgCount;
                stackArgs[0] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src0Slot]);
                if (argCount >= 2u) {
                    stackArgs[1] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src1Slot]);
                }
                if (argCount >= 3u) {
                    stackArgs[2] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src2Slot]);
                }
                argsPtr = stackArgs.data();
            } else {
                if (!cold.bytecode || cold.bytecode->operands.empty()) {
                    return failOp(runtime::ErrorCode::InvalidProgram, "CallPolledFast missing operands metadata");
                }
                const auto& ops = cold.bytecode->operands;
                argCount = static_cast<std::uint32_t>(ops.size());
                if (argCount <= stackArgs.size()) {
                    for (std::uint32_t i = 0; i < argCount; ++i) {
                        stackArgs[i] = runtime::detachFrameSliceValue(
                            ctx->frameBase[execFunc.registerOffset + ops[i]]);
                    }
                    argsPtr = stackArgs.data();
                } else {
                    heapArgs.resize(argCount);
                    for (std::uint32_t i = 0; i < argCount; ++i) {
                        heapArgs[i] = runtime::detachFrameSliceValue(
                            ctx->frameBase[execFunc.registerOffset + ops[i]]);
                    }
                    argsPtr = heapArgs.data();
                }
            }
        }

        if (!requireFullSpanBudget()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= fi.span;
        runtime::Value callResult{};
        const std::uint32_t subStatus = session.directInvokeFn(
            session.moduleImpl,
            ctx,
            targetSymId,
            argsPtr,
            argCount,
            &callResult,
            session.forceSimulateArm64);
        invalidateJitArrayCaches(*ctx);
        if (subStatus != 0u) {
            appendJitCallerErrorFrame(
                session,
                *ctx->compiledFn->location,
                *ctx->compiledFn->execFunc,
                flatIdx);
            ctx->errorCodeRaw = subStatus;
            ctx->errorFlatIndex = flatIdx;
            return subStatus;
        }
        if (fi.dstSlot != runtime::detail::InvalidCompactReg) {
            storeJitFlatFrameSlot(
                execFunc,
                ctx->frameBase,
                session.flatBankId,
                session.frameOffset,
                fi.dstSlot,
                std::move(callResult));
        }
        return 0u;
    }
    if (session.flatBankId == 0u || session.values == nullptr) {
        return failOp(runtime::ErrorCode::InvalidProgram, "flat stack bank is inactive in StencilJit");
    }
    if (!requireFullSpanBudget()) {
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }

    runtime::Value* frameBase = ctx->frameBase;
    const std::size_t frameOffset = session.frameOffset;

    switch (fi.flatOpcode) {
    case FO::InitStructSlice: {
        const std::uint16_t sid = fi.aux0Slot;
        const auto* sc = runtime::getFlatStructSchema(sid);
        const std::uint32_t payOff = fi.auxImm32;
        if (!sc || payOff + sc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + payOff > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "invalid InitStructSlice metadata");
        }
        std::memcpy(
            static_cast<void*>(frameBase + payOff),
            static_cast<const void*>(sc->prototypeLeaves.data()),
            static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            static_cast<semantic::SymbolId>(fi.integerImmediate),
            session.flatBankId,
            static_cast<std::uint32_t>(frameOffset + payOff),
            sid);
        return 0u;
    }
    case FO::CopyStructSlice: {
        const std::uint16_t sid = fi.aux0Slot;
        const std::uint32_t dstPayOff = fi.auxImm32;
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (!sc || dstPayOff + sc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + dstPayOff > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "invalid CopyStructSlice metadata");
        }
        const runtime::Value& srcVal = frameBase[fi.src0Slot];
        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + dstPayOff);
        if (srcVal.rawTag() == runtime::ValueTag::Struct &&
            srcVal.asStructUnchecked().isFrameSlice() &&
            srcVal.asStructUnchecked().frameSchemaId() == sid &&
            srcVal.asStructUnchecked().storage.frameSliceBank() == session.flatBankId) {
            const std::uint32_t srcAbs = srcVal.asStructUnchecked().storage.frameSliceOffset();
            if (srcAbs != dstAbs) {
                std::memmove(
                    static_cast<void*>(session.values->data() + dstAbs),
                    static_cast<const void*>(session.values->data() + srcAbs),
                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
            }
        } else if (!runtime::extractStructLeavesToBuffer(srcVal, *sc, frameBase + dstPayOff)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "CopyStructSlice source mismatch");
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            static_cast<semantic::SymbolId>(fi.integerImmediate),
            session.flatBankId,
            dstAbs,
            sid);
        return 0u;
    }
    case FO::LoadStructSliceField: {
        const runtime::Value& recv = frameBase[fi.src0Slot];
        if (recv.rawTag() != runtime::ValueTag::Struct) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStructSliceField invalid slice");
        }
        const std::uint16_t subSid = fi.aux1Slot;
        const auto subTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        if (!recv.asStructUnchecked().isFrameSlice()) {
            const auto* recvSc = runtime::findFlatStructSchemaByTypeId(recv.asStructUnchecked().typeId);
            if (!recvSc || fi.aux0Slot >= recvSc->flatSlotCount) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStructSliceField invalid detached struct");
            }
            std::vector<runtime::Value> tmpLeaves(recvSc->flatSlotCount);
            if (!runtime::extractStructLeavesToBuffer(recv, *recvSc, tmpLeaves.data())) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStructSliceField detached extract failed");
            }
            if (subSid == 0u) {
                ctx->remainingBudget -= fi.span;
                frameBase[fi.dstSlot] = tmpLeaves[fi.aux0Slot];
                return 0u;
            }
            const auto* subSc = runtime::getFlatStructSchema(subSid);
            if (!subSc || static_cast<std::uint32_t>(fi.aux0Slot) + subSc->flatSlotCount > recvSc->flatSlotCount) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField invalid detached sub-slice");
            }
            if (fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
                ctx->remainingBudget -= fi.span;
                frameBase[fi.dstSlot] = runtime::reconstructDetachedStructFromLeaves(*subSc, tmpLeaves.data() + fi.aux0Slot);
                return 0u;
            }
            if (fi.auxImm32 + subSc->flatSlotCount > execFunc.totalFrameSlots ||
                frameOffset + fi.auxImm32 > runtime::StructStorageHandle::FrameOffsetMask) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField invalid sub-slice");
            }
            const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + fi.auxImm32);
            for (std::uint16_t k = 0; k < subSc->flatSlotCount; ++k) {
                (*session.values)[dstAbs + k] = tmpLeaves[fi.aux0Slot + k];
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                subTid, session.flatBankId, dstAbs, subSid);
            return 0u;
        }
        if (recv.asStructUnchecked().storage.frameSliceBank() != session.flatBankId) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStructSliceField invalid slice");
        }
        const std::uint64_t baseAbs64 =
            static_cast<std::uint64_t>(recv.asStructUnchecked().storage.frameSliceOffset()) + fi.aux0Slot;
        if (subSid == 0u) {
            if (baseAbs64 >= session.values->size()) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField out of bounds");
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = (*session.values)[static_cast<std::size_t>(baseAbs64)];
            return 0u;
        }
        if (fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
            if (baseAbs64 > runtime::StructStorageHandle::FrameOffsetMask) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField offset overflow");
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                subTid, session.flatBankId, static_cast<std::uint32_t>(baseAbs64), subSid);
            return 0u;
        }
        const auto* subSc = runtime::getFlatStructSchema(subSid);
        if (!subSc || fi.auxImm32 + subSc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + fi.auxImm32 > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStructSliceField invalid sub-slice");
        }
        const std::uint32_t baseAbs = static_cast<std::uint32_t>(baseAbs64);
        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + fi.auxImm32);
        if (dstAbs != baseAbs) {
            std::memmove(
                static_cast<void*>(session.values->data() + dstAbs),
                static_cast<const void*>(session.values->data() + baseAbs),
                static_cast<std::size_t>(subSc->flatSlotCount) * sizeof(runtime::Value));
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            subTid, session.flatBankId, dstAbs, subSid);
        return 0u;
    }
    case FO::StoreStructSliceField: {
        const std::uint16_t sid = fi.aux0Slot;
        const std::uint16_t fIdx = fi.aux1Slot;
        const std::uint32_t dstPayOff = fi.auxImm32;
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (!sc || fIdx >= sc->directFields.size() || dstPayOff + sc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + dstPayOff > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "StoreStructSliceField invalid metadata");
        }
        const runtime::Value& recv = frameBase[fi.src0Slot];
        const runtime::Value& rhs = frameBase[fi.src1Slot];
        runtime::Value* dstLeaves = frameBase + dstPayOff;
        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + dstPayOff);
        if (recv.rawTag() == runtime::ValueTag::Struct &&
            recv.asStructUnchecked().isFrameSlice() &&
            recv.asStructUnchecked().frameSchemaId() == sid &&
            recv.asStructUnchecked().storage.frameSliceBank() == session.flatBankId) {
            const std::uint32_t srcAbs = recv.asStructUnchecked().storage.frameSliceOffset();
            if (srcAbs != dstAbs) {
                std::memmove(
                    static_cast<void*>(dstLeaves),
                    static_cast<const void*>(session.values->data() + srcAbs),
                    static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
            }
        } else if (!runtime::extractStructLeavesToBuffer(recv, *sc, dstLeaves)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreStructSliceField receiver mismatch");
        }
        const auto& fd = sc->directFields[fIdx];
        if (fd.subSchemaId == 0u) {
            dstLeaves[fd.leafOffset] = rhs;
        } else {
            const auto* subSc = runtime::getFlatStructSchema(fd.subSchemaId);
            if (!subSc || !runtime::extractStructLeavesToBuffer(rhs, *subSc, dstLeaves + fd.leafOffset)) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreStructSliceField field mismatch");
            }
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            static_cast<semantic::SymbolId>(fi.integerImmediate),
            session.flatBankId,
            dstAbs,
            sid);
        return 0u;
    }
    case FO::StoreLocalStructFieldInPlace: {
        const std::uint32_t locPayOff = fi.auxImm32;
        const std::uint16_t leafOff = fi.aux0Slot;
        const std::uint16_t subSid = fi.aux1Slot;
        const runtime::Value& rhs = frameBase[fi.src1Slot];
        if (subSid == 0u) {
            frameBase[locPayOff + leafOff] = rhs;
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        const auto* subSc = runtime::getFlatStructSchema(subSid);
        if (!subSc || !runtime::extractStructLeavesToBuffer(rhs, *subSc, frameBase + locPayOff + leafOff)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreLocalStructFieldInPlace sub-struct mismatch");
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    case FO::InitStackRefCell: {
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.aux0Slot >= mirror.stackRefTemplates.size() || fi.auxImm32 >= execFunc.totalFrameSlots) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "InitStackRefCell invalid template");
        }
        const auto& srt = mirror.stackRefTemplates[fi.aux0Slot];
        const std::uint32_t cellOff = fi.auxImm32;
        if (srt.flatSchemaId == 0u) {
            frameBase[cellOff] = srt.scalarDefaultValue;
        } else {
            const auto* sc = runtime::getFlatStructSchema(srt.flatSchemaId);
            if (!sc || cellOff + sc->flatSlotCount > execFunc.totalFrameSlots) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "InitStackRefCell invalid struct slice");
            }
            std::memcpy(
                static_cast<void*>(frameBase + cellOff),
                static_cast<const void*>(sc->prototypeLeaves.data()),
                static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::ObjectRef::makeStackSlotRef(
            static_cast<std::uint32_t>(frameOffset + cellOff),
            session.flatBankId,
            srt.flatSchemaId);
        return 0u;
    }
    case FO::BindLocalStackRef: {
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::ObjectRef::makeStackSlotRef(
            static_cast<std::uint32_t>(frameOffset + fi.auxImm32),
            session.flatBankId,
            fi.aux0Slot);
        return 0u;
    }
    case FO::WritebackLocalStackRef: {
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    case FO::LoadStackRefCell: {
        const runtime::Value& recvVal = frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "LoadStackRefCell invalid ref");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        const std::uint16_t sid = fi.aux0Slot;
        if (!ref.isStackSlotRef()) {
            const auto fv = session.heap ? session.heap->fieldGet(ref, 0) : std::nullopt;
            if (!fv) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "LoadStackRefCell dangling heap ref");
            }
            if (sid == 0u || fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
                ctx->remainingBudget -= fi.span;
                frameBase[fi.dstSlot] = *fv;
                return 0u;
            }
            const auto* sc = runtime::getFlatStructSchema(sid);
            if (!sc || fi.auxImm32 + sc->flatSlotCount > execFunc.totalFrameSlots ||
                frameOffset + fi.auxImm32 > runtime::StructStorageHandle::FrameOffsetMask) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStackRefCell invalid payload");
            }
            const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + fi.auxImm32);
            if (!runtime::extractStructLeavesToBuffer(*fv, *sc, session.values->data() + dstAbs)) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "LoadStackRefCell heap struct mismatch");
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                static_cast<semantic::SymbolId>(fi.integerImmediate), session.flatBankId, dstAbs, sid);
            return 0u;
        }
        if (ref.generation != session.flatBankId) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "LoadStackRefCell invalid ref");
        }
        if (sid == 0u) {
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = (*session.values)[ref.slot];
            return 0u;
        }
        const auto innerTid = static_cast<semantic::SymbolId>(fi.integerImmediate);
        if (fi.auxImm32 == runtime::detail::InvalidStreamOffset) {
            if (ref.slot > runtime::StructStorageHandle::FrameOffsetMask) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStackRefCell slice offset overflow");
            }
            ctx->remainingBudget -= fi.span;
            frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                innerTid, session.flatBankId, ref.slot, sid);
            return 0u;
        }
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (!sc || fi.auxImm32 + sc->flatSlotCount > execFunc.totalFrameSlots ||
            frameOffset + fi.auxImm32 > runtime::StructStorageHandle::FrameOffsetMask) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "LoadStackRefCell invalid payload");
        }
        const std::uint32_t dstAbs = static_cast<std::uint32_t>(frameOffset + fi.auxImm32);
        if (dstAbs != ref.slot) {
            std::memmove(
                static_cast<void*>(session.values->data() + dstAbs),
                static_cast<const void*>(session.values->data() + ref.slot),
                static_cast<std::size_t>(sc->flatSlotCount) * sizeof(runtime::Value));
        }
        ctx->remainingBudget -= fi.span;
        frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
            innerTid, session.flatBankId, dstAbs, sid);
        return 0u;
    }
    case FO::StoreStackRefCell: {
        const runtime::Value& recvVal = frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "StoreStackRefCell invalid ref");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        const runtime::Value& rhs = frameBase[fi.src1Slot];
        if (!ref.isStackSlotRef()) {
            if (!session.heap) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "StoreStackRefCell missing heap");
            }
            runtime::RuntimeError setErr;
            if (!session.heap->fieldSet(ref, 0, runtime::detachFrameSliceValue(rhs), &setErr)) {
                return setHelperError(
                    ctx,
                    flatIdx,
                    setErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::InvalidProgram : setErr.code,
                    setErr.message.empty() ? "StoreStackRefCell heap write failed" : setErr.message);
            }
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        if (ref.generation != session.flatBankId) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "StoreStackRefCell invalid ref");
        }
        const std::uint16_t sid = fi.aux0Slot;
        if (sid == 0u) {
            (*session.values)[ref.slot] = runtime::detachFrameSliceValue(rhs);
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (!sc || !runtime::extractStructLeavesToBuffer(rhs, *sc, session.values->data() + ref.slot)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreStackRefCell struct mismatch");
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    case FO::StoreStackRefStructFieldInPlace: {
        const runtime::Value& recvVal = frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef ||
            !recvVal.u_.objectRef.isStackSlotRef() ||
            recvVal.u_.objectRef.generation != session.flatBankId) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "StoreStackRefStructFieldInPlace invalid ref");
        }
        const std::uint32_t dstAbs = recvVal.u_.objectRef.slot + fi.aux0Slot;
        const std::uint16_t subSid = fi.aux1Slot;
        const runtime::Value& rhs = frameBase[fi.src1Slot];
        if (subSid == 0u) {
            (*session.values)[dstAbs] = runtime::detachFrameSliceValue(rhs);
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        const auto* subSc = runtime::getFlatStructSchema(subSid);
        if (!subSc || !runtime::extractStructLeavesToBuffer(rhs, *subSc, session.values->data() + dstAbs)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "StoreStackRefStructFieldInPlace sub-struct mismatch");
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    default:
        return failOp(runtime::ErrorCode::InvalidProgram, "unsupported flat struct/ref op in StencilJit");
    }
}

bool normalizeAndValidateJitInterceptedReturn(
    const runtime::ProgramImage::FunctionLocation& targetLoc,
    const runtime::ProgramImage* program,
    runtime::ManagedHeap* heap,
    runtime::IExecutionPolicy* executionPolicy,
    const runtime::Value& interceptedResult,
    runtime::Value& outVal,
    runtime::ErrorCode& outCode,
    std::string& outMsg) {
    if (!targetLoc.function) {
        outVal = runtime::detachFrameSliceValue(interceptedResult);
        return true;
    }
    const auto& calleeFn = *targetLoc.function;
    if (calleeFn.returnType == semantic::PrimitiveType::Void) {
        outVal = runtime::Value{std::monostate{}};
        return true;
    }
    if (std::holds_alternative<std::monostate>(interceptedResult)) {
        outVal = targetLoc.module
            ? runtime::interp_detail::defaultValue(*targetLoc.module, calleeFn.returnType, calleeFn.returnTypeId)
            : runtime::Value{std::monostate{}};
        return true;
    }
    runtime::Value detached = runtime::detachFrameSliceValue(interceptedResult);
    if (const auto* ref = std::get_if<runtime::ObjectRef>(&detached)) {
        runtime::ManagedHeap* targetHeap = executionPolicy
            ? executionPolicy->resolveHeap(*ref, heap)
            : heap;
        if (!targetHeap || !targetHeap->isAlive(*ref)) {
            outCode = runtime::ErrorCode::InvalidObjectReference;
            outMsg = "intercepted return value contains an invalid managed object reference";
            return false;
        }
    }
    if (runtime::valueType(detached) != calleeFn.returnType) {
        outCode = runtime::ErrorCode::TypeMismatch;
        outMsg = "intercepted return value type does not match callee signature";
        return false;
    }
    const auto expectedType = calleeFn.returnType;
    const auto expectedTid = calleeFn.returnTypeId;
    if (expectedType == semantic::PrimitiveType::Int) {
        if (!fitsInt32(detached.asInt64Unchecked())) {
            outCode = runtime::ErrorCode::TypeMismatch;
            outMsg = "intercepted int return value exceeds 32-bit range";
            return false;
        }
    } else if (expectedType == semantic::PrimitiveType::Array) {
        if (!std::holds_alternative<runtime::NullArray>(detached) && expectedTid != 0) {
            const auto* ref = std::get_if<runtime::ObjectRef>(&detached);
            runtime::ManagedHeap* targetHeap = (ref && executionPolicy)
                ? executionPolicy->resolveHeap(*ref, heap)
                : heap;
            const auto actualTid = (ref && targetHeap)
                ? targetHeap->arrayTypeId(*ref)
                : std::optional<semantic::SymbolId>{};
            if (!actualTid || *actualTid != expectedTid) {
                outCode = runtime::ErrorCode::TypeMismatch;
                outMsg = "intercepted return value has the wrong runtime array type";
                return false;
            }
        }
    } else if (expectedType == semantic::PrimitiveType::Struct) {
        if (expectedTid != 0 && runtime::structValueTypeId(detached) != expectedTid) {
            outCode = runtime::ErrorCode::TypeMismatch;
            outMsg = "intercepted return value has the wrong runtime struct type";
            return false;
        }
    } else if (expectedType == semantic::PrimitiveType::Object) {
        if (!std::holds_alternative<runtime::NullObject>(detached) && expectedTid != 0) {
            const auto* ref = std::get_if<runtime::ObjectRef>(&detached);
            runtime::ManagedHeap* targetHeap = (ref && executionPolicy)
                ? executionPolicy->resolveHeap(*ref, heap)
                : heap;
            const auto actualTid = (ref && targetHeap)
                ? targetHeap->objectTypeId(*ref)
                : std::optional<semantic::SymbolId>{};
            if (!actualTid) {
                outCode = runtime::ErrorCode::InvalidObjectReference;
                outMsg = "intercepted return value contains an invalid managed object reference";
                return false;
            }
            if (*actualTid != expectedTid) {
                bool assignable = false;
                if (program != nullptr) {
                    auto curTid = *actualTid;
                    for (std::size_t depth = 0; curTid != 0 && depth < 256; ++depth) {
                        const auto* t = program->resolveType(curTid);
                        if (!t) break;
                        for (const auto& iface : t->interfaceDispatchMaps) {
                            if (iface.interfaceTypeId == expectedTid) {
                                assignable = true;
                                break;
                            }
                        }
                        if (assignable || t->baseTypeId == expectedTid) {
                            assignable = true;
                            break;
                        }
                        curTid = t->baseTypeId;
                    }
                }
                if (!assignable) {
                    outCode = runtime::ErrorCode::TypeMismatch;
                    outMsg = "intercepted return value has the wrong runtime object type";
                    return false;
                }
            }
        }
    } else if (expectedType == semantic::PrimitiveType::Enum) {
        if (expectedTid != 0) {
            runtime::EnumValue ev{};
            if (!detached.tryGetEnum(ev) || (ev.typeId != 0 && ev.typeId != expectedTid)) {
                outCode = runtime::ErrorCode::TypeMismatch;
                outMsg = "intercepted return value has the wrong runtime enum type";
                return false;
            }
        }
    } else if (expectedType == semantic::PrimitiveType::Handle) {
        if (expectedTid != 0) {
            runtime::NativeHandle h{};
            if (!detached.tryGetNativeHandle(h) || (h.typeId != 0 && h.typeId != expectedTid)) {
                outCode = runtime::ErrorCode::TypeMismatch;
                outMsg = "intercepted return value has the wrong runtime handle type";
                return false;
            }
        }
    }
    outVal = std::move(detached);
    return true;
}

// Helper for InlineCallEnter (tracks recursion limit, functionCalls, maximumCallDepth, and frame setup)
std::uint32_t jitInlineCallEnterHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    auto& session = *ctx->session;

    if (ctx->remainingBudget < 1u) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    if (session.callDepth >= session.limits.recursionLimit) {
        ctx->remainingBudget -= 1u;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::RecursionLimitExceeded, "recursion limit exceeded");
    }

    const std::uint32_t nArgs = fi.inlineArgCount;
    const std::uint32_t nLocals = fi.aux0Slot;
    if (session.executionPolicy != nullptr && flatIdx < execFunc.flatCold.size()) {
        const auto enterCold = execFunc.resolveCold(flatIdx);
        const auto* targetLoc = enterCold.directTarget ? enterCold.directTarget : enterCold.inlinedCallee;
        if (targetLoc != nullptr && targetLoc->function != nullptr) {
            std::array<runtime::Value, 4> policyArgs{};
            if (nArgs >= 1u) policyArgs[0] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src0Slot]);
            if (nArgs >= 2u) policyArgs[1] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src1Slot]);
            if (nArgs >= 3u) policyArgs[2] = runtime::detachFrameSliceValue(ctx->frameBase[fi.src2Slot]);
            if (nArgs >= 4u) policyArgs[3] = runtime::detachFrameSliceValue(ctx->frameBase[fi.aux1Slot]);
            bool intercepted = false;
            runtime::Value interceptedResult{};
            runtime::RuntimeError policyError{};
            const auto& calleeFn = *targetLoc->function;
            if (!session.executionPolicy->interceptCall(
                    calleeFn.symbolId,
                    runtime::ValueSpan(policyArgs.data(), nArgs),
                    intercepted,
                    interceptedResult,
                    policyError)) {
                ctx->remainingBudget -= 1u;
                const auto errCode = policyError.code == runtime::ErrorCode::None
                    ? runtime::ErrorCode::CapabilityDenied
                    : policyError.code;
                const std::string errMsg = policyError.message.empty()
                    ? std::string("call intercepted and rejected by execution policy")
                    : policyError.message;
                return setHelperError(ctx, flatIdx, errCode, errMsg);
            }
            if (intercepted) {
                ctx->remainingBudget -= 1u;
                ++session.stats.functionCalls;
                if (session.callDepth + 1 > session.stats.maximumCallDepth) {
                    session.stats.maximumCallDepth = session.callDepth + 1;
                }
                runtime::Value resolvedRet;
                runtime::ErrorCode retErrCode = runtime::ErrorCode::TypeMismatch;
                std::string retErrMsg;
                if (!normalizeAndValidateJitInterceptedReturn(
                        *targetLoc,
                        session.program,
                        session.heap,
                        session.executionPolicy,
                        interceptedResult,
                        resolvedRet,
                        retErrCode,
                        retErrMsg)) {
                    return setHelperError(ctx, flatIdx, retErrCode, std::move(retErrMsg));
                }
                if (fi.storeSlot != runtime::detail::InvalidCompactReg) {
                    storeJitFlatFrameSlot(
                        execFunc,
                        ctx->frameBase,
                        session.flatBankId,
                        session.frameOffset,
                        fi.storeSlot,
                        std::move(resolvedRet));
                }
                ctx->nextFlatOffsetOut = 2u;
                return 0u;
            }
        }
    }

    ctx->remainingBudget -= 1u;
    ++session.stats.functionCalls;
    if (session.callDepth + 1 > session.stats.maximumCallDepth) {
        session.stats.maximumCallDepth = session.callDepth + 1;
    }

    runtime::Value* const calleeBase = ctx->frameBase + fi.dstSlot;
    if ((fi.arithFlags & runtime::detail::FlatInstruction::FlagFastParamPrologue) != 0u &&
        ctx->remainingBudget >= 2u * nArgs) {
        runtime::Value* const calleeLocals = calleeBase + nArgs;
        if (nArgs >= 1u) calleeLocals[0] = ctx->frameBase[fi.src0Slot];
        if (nArgs >= 2u) calleeLocals[1] = ctx->frameBase[fi.src1Slot];
        if (nArgs >= 3u) calleeLocals[2] = ctx->frameBase[fi.src2Slot];
        if (nArgs >= 4u) calleeLocals[3] = ctx->frameBase[fi.aux1Slot];
        if (nLocals > nArgs) {
            std::memset(static_cast<void*>(calleeLocals + nArgs), 0, (nLocals - nArgs) * sizeof(runtime::Value));
        }
        ctx->remainingBudget -= 2u * nArgs;
        ctx->nextFlatOffsetOut = 1u; // jump to flatIdx + 1 + nArgs
        return 0u;
    }
    if (nArgs >= 1u) calleeBase[0] = ctx->frameBase[fi.src0Slot];
    if (nArgs >= 2u) calleeBase[1] = ctx->frameBase[fi.src1Slot];
    if (nArgs >= 3u) calleeBase[2] = ctx->frameBase[fi.src2Slot];
    if (nArgs >= 4u) calleeBase[3] = ctx->frameBase[fi.aux1Slot];
    if (nLocals != 0u) {
        std::memset(static_cast<void*>(calleeBase + nArgs), 0, nLocals * sizeof(runtime::Value));
    }
    ctx->nextFlatOffsetOut = 0u;
    return 0u;
}

// Helper for mid-superinstruction budget exhaustion (exact partial sub-instruction accounting)
std::uint32_t jitPartialSuperBudgetHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];

    if ((fi.arithFlags & runtime::detail::FlatInstruction::FlagPreJump1) != 0u) {
        if (ctx->remainingBudget == 0u) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        ctx->remainingBudget -= 1u;
        ++ctx->branchesTaken;
        flatIdx = fi.auxImm32;
    }

    if (fi.flatOpcode == runtime::detail::FlatOpcode::TermJumpNoArgs && fi.span == 2u) {
        if (ctx->remainingBudget >= 1u) {
            ctx->remainingBudget = 0;
            ++ctx->branchesTaken;
            flatIdx = fi.auxImm32;
        }
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }

    // Check if a binary arithmetic sub-instruction traps with overflow/div0 BEFORE running out of budget
    const auto& curFi = execFunc.flatStream[flatIdx];
    const auto checkIntTrapAtSubStep = [&](
        runtime::detail::QuickOpcode qop,
        bool checked,
        std::int64_t lhs,
        std::int64_t rhs,
        std::uint64_t stepNeeded) -> std::uint32_t {
        if (ctx->remainingBudget < stepNeeded) {
            ctx->remainingBudget = 0;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
        }
        if (qop == runtime::detail::QuickOpcode::DivideInt) {
            if (rhs == 0) {
                ctx->remainingBudget -= stepNeeded;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::DivisionByZero, "division by zero");
            }
            if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) {
                ctx->remainingBudget -= stepNeeded;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::IntegerOverflow, "integer division overflow");
            }
        } else if (qop == runtime::detail::QuickOpcode::RemainderInt) {
            if (rhs == 0) {
                ctx->remainingBudget -= stepNeeded;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::DivisionByZero, "remainder by zero");
            }
        } else if (checked) {
            std::int64_t r = 0;
            bool ovf = false;
            if (qop == runtime::detail::QuickOpcode::AddInt) {
                r = lhs + rhs;
                ovf = !fitsInt32(r);
            } else if (qop == runtime::detail::QuickOpcode::SubtractInt) {
                r = lhs - rhs;
                ovf = !fitsInt32(r);
            } else if (qop == runtime::detail::QuickOpcode::MultiplyInt) {
                r = lhs * rhs;
                ovf = !fitsInt32(r);
            }
            if (ovf) {
                ctx->remainingBudget -= stepNeeded;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::IntegerOverflow, "integer overflow");
            }
        }
        return 0u;
    };

    switch (curFi.flatOpcode) {
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntConstStore:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntConstStoreElided:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntConstStoreJump: {
        std::int64_t lhs = 0;
        if (ctx->frameBase[curFi.src0Slot].tryGetInt64(lhs)) {
            if (const auto trap = checkIntTrapAtSubStep(curFi.binaryOpcode1, curFi.checked1(), lhs, curFi.integerImmediate, 3u)) {
                return trap;
            }
        }
        break;
    }
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntLocalStore:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntLocalStoreElided:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntLocalStoreJump: {
        std::int64_t lhs = 0, rhs = 0;
        if (ctx->frameBase[curFi.src0Slot].tryGetInt64(lhs) &&
            ctx->frameBase[curFi.src1Slot].tryGetInt64(rhs)) {
            if (const auto trap = checkIntTrapAtSubStep(curFi.binaryOpcode1, curFi.checked1(), lhs, rhs, 3u)) {
                return trap;
            }
        }
        break;
    }
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntRegStore:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntRegStoreElided:
    case runtime::detail::FlatOpcode::SuperBinaryLocalIntRegStoreJump: {
        std::int64_t lhs = 0, rhs = 0;
        if (ctx->frameBase[curFi.src0Slot].tryGetInt64(lhs) &&
            ctx->frameBase[curFi.src1Slot].tryGetInt64(rhs)) {
            if (const auto trap = checkIntTrapAtSubStep(curFi.binaryOpcode1, curFi.checked1(), lhs, rhs, 2u)) {
                return trap;
            }
        }
        break;
    }
    default:
        break;
    }

    ctx->remainingBudget = 0;
    return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
}

[[nodiscard]] std::uint8_t computeUnexecutedAfterOp(
    const runtime::detail::ExecutionFunction& execFunc,
    std::size_t flatIdx) noexcept {
    const auto& fi = execFunc.flatStream[flatIdx];
    if (fi.span <= 1u || flatIdx >= execFunc.flatCold.size()) return 0u;
    const auto cold = execFunc.resolveCold(flatIdx);
    using FO = runtime::detail::FlatOpcode;
    switch (fi.flatOpcode) {
    case FO::SuperBinaryLocalIntConst:
    case FO::SuperBinaryLocalIntLocal:
    case FO::SuperBinaryLocalIntReg:
        return 0u;
    case FO::SuperBinaryLocalIntConstStore:
    case FO::SuperBinaryLocalIntConstStoreElided:
    case FO::SuperBinaryLocalIntLocalStore:
    case FO::SuperBinaryLocalIntLocalStoreElided:
    case FO::SuperBinaryLocalIntRegStore:
    case FO::SuperBinaryLocalIntRegStoreElided:
        return 1u;
    case FO::SuperBinaryLocalIntConstStoreJump:
    case FO::SuperBinaryLocalIntLocalStoreJump:
    case FO::SuperBinaryLocalIntRegStoreJump:
        return 2u;
    default:
        break;
    }
    const auto* activeExec = (cold.inlinedCallee != nullptr && cold.directExecution != nullptr)
        ? cold.directExecution
        : &execFunc;
    if (cold.bytecode != nullptr && cold.blockIndex < activeExec->blocks.size()) {
        const auto& blk = activeExec->blocks[cold.blockIndex];
        if (blk.bytecode != nullptr) {
            const std::size_t start = cold.instructionIndexInBlock;
            const std::size_t limit = std::min<std::size_t>(
                blk.bytecode->instructions.size(), start + fi.span);
            for (std::size_t m = start; m < limit; ++m) {
                if (&blk.bytecode->instructions[m] == cold.bytecode) {
                    const std::size_t done = (m - start) + 1u;
                    if (done <= fi.span) {
                        return static_cast<std::uint8_t>(fi.span - done);
                    }
                }
            }
        }
    }
    return 0u;
}

[[nodiscard]] bool isStencilSupportedFlatOp(
    const runtime::detail::ExecutionFunction& execFunc,
    const runtime::detail::FlatInstruction& fi) noexcept {
    using FO = runtime::detail::FlatOpcode;
    const auto validSlot = [&](std::uint16_t s) noexcept {
        return s < execFunc.totalFrameSlots;
    };
    const auto validQuickIntOp = [](runtime::detail::QuickOpcode op) noexcept {
        using QO = runtime::detail::QuickOpcode;
        return (op >= QO::AddInt && op <= QO::NotEqualInt);
    };
    const auto validIntCmpOp = [](runtime::detail::QuickOpcode op) noexcept {
        using QO = runtime::detail::QuickOpcode;
        return (op >= QO::LessInt && op <= QO::NotEqualInt);
    };
    const auto validDoubleArithOp = [](runtime::detail::QuickOpcode op) noexcept {
        using QO = runtime::detail::QuickOpcode;
        return (op >= QO::AddDouble && op <= QO::DivideDouble);
    };
    const auto validFloatArithOp = [](runtime::detail::QuickOpcode op) noexcept {
        using QO = runtime::detail::QuickOpcode;
        return (op >= QO::AddFloat && op <= QO::DivideFloat);
    };

    switch (fi.flatOpcode) {
    case FO::MoveSlot:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::SuperMoveParamStoreLocal:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) && validSlot(fi.storeSlot);
    case FO::SuperMoveParamStoreLocalElided:
        return validSlot(fi.src0Slot) && validSlot(fi.storeSlot);
    case FO::ConstantInt:
        return validSlot(fi.dstSlot) && fitsInt32(fi.integerImmediate);
    case FO::ConstantDouble:
    case FO::ConstantBool:
    case FO::ConstantLong:
    case FO::SuperConstFloat:
        return validSlot(fi.dstSlot);
    case FO::SuperConstFloatStoreLocal:
        return validSlot(fi.storeSlot);
    case FO::AddIntChecked:
    case FO::AddIntUnchecked:
    case FO::SubIntChecked:
    case FO::SubIntUnchecked:
    case FO::MulIntChecked:
    case FO::MulIntUnchecked:
    case FO::DivInt:
    case FO::RemInt:
    case FO::LessInt:
    case FO::LessOrEqualInt:
    case FO::GreaterInt:
    case FO::GreaterOrEqualInt:
    case FO::EqualInt:
    case FO::NotEqualInt:
    case FO::AddIntTrustedChecked:
    case FO::AddIntTrustedUnchecked:
    case FO::SubIntTrustedChecked:
    case FO::SubIntTrustedUnchecked:
    case FO::MulIntTrustedChecked:
    case FO::MulIntTrustedUnchecked:
    case FO::LessIntTrusted:
    case FO::AddDouble:
    case FO::SubDouble:
    case FO::MulDouble:
    case FO::DivDouble:
    case FO::LessDouble:
    case FO::LessOrEqualDouble:
    case FO::GreaterDouble:
    case FO::GreaterOrEqualDouble:
    case FO::AddDoubleTrusted:
    case FO::SubDoubleTrusted:
    case FO::MulDoubleTrusted:
    case FO::DivDoubleTrusted:
    case FO::AddFloat:
    case FO::SubFloat:
    case FO::MulFloat:
    case FO::DivFloat:
    case FO::LessFloat:
    case FO::LessOrEqualFloat:
    case FO::GreaterFloat:
    case FO::GreaterOrEqualFloat:
    case FO::EqualFloat:
    case FO::NotEqualFloat:
    case FO::AddFloatTrusted:
    case FO::SubFloatTrusted:
    case FO::MulFloatTrusted:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::BinaryUIntFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::NegateIntChecked:
    case FO::NegateIntUnchecked:
    case FO::ConvertNumericFast:
    case FO::NegateDouble:
    case FO::NegateFloat:
    case FO::LogicalNot:
    case FO::ConvertIntToLongFast:
    case FO::ConvertIntToDoubleFast:
    case FO::ConvertLongToDoubleFast:
    case FO::ConvertIntToFloatFast:
    case FO::ConvertFloatToDoubleFast:
    case FO::ConvertDoubleToFloatFast:
    case FO::ConvertFloatToIntFast:
    case FO::ConvertLongToIntFast:
    case FO::ArrayLengthFast:
    case FO::CheckNotNullFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::NewArrayFast: {
        if (!validSlot(fi.dstSlot) || !validSlot(fi.src0Slot)) return false;
        const auto elemType = static_cast<semantic::PrimitiveType>(
            static_cast<std::uint8_t>(fi.binaryOpcode1));
        if (fi.aux0Slot == 0 && elemType == semantic::PrimitiveType::Struct) {
            const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
            const auto cold = execFunc.resolveCold(idx);
            if (idx >= execFunc.flatCold.size() || cold.bytecode == nullptr) {
                return false;
            }
            const auto elemTid = cold.bytecode->elementTypeId;
            return runtime::isBlittableStructType(elemTid) ||
                   runtime::findFlatStructSchemaByTypeId(elemTid) != nullptr;
        }
        return true;
    }
    case FO::SuperBinaryLocalIntConst:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) &&
               validSlot(fi.aux0Slot) && validSlot(fi.aux1Slot) &&
               fitsInt32(fi.integerImmediate) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntConstStore:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) &&
               validSlot(fi.aux0Slot) && validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               fitsInt32(fi.integerImmediate) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntConstStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.storeSlot) &&
               fitsInt32(fi.integerImmediate) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntLocal:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) && validSlot(fi.aux1Slot) &&
               validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntLocalStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) &&
               validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntLocalStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntReg:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux1Slot) &&
               validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntRegStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntRegStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validQuickIntOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalIntRegChainStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.src2Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) &&
               validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validQuickIntOp(fi.binaryOpcode1) && validQuickIntOp(fi.binaryOpcode2);
    case FO::SuperBinaryLocalIntRegChainStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.src2Slot) &&
               validSlot(fi.storeSlot) &&
               validQuickIntOp(fi.binaryOpcode1) && validQuickIntOp(fi.binaryOpcode2);
    case FO::SuperBinaryLocalIntConstStoreJump:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) &&
               fitsInt32(fi.integerImmediate) && validQuickIntOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::SuperBinaryLocalIntLocalStoreJump:
    case FO::SuperBinaryLocalIntRegStoreJump:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.dstSlot) &&
               validQuickIntOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::SuperLessLocalConstBranch:
        return validSlot(fi.src0Slot) && fitsInt32(fi.integerImmediate) &&
               validIntCmpOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::SuperLessLocalRegBranch:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validIntCmpOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::SuperLessLocalArrayLengthBranch:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validIntCmpOp(fi.binaryOpcode1) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::SuperBinaryLocalDoubleConstStore:
        return validSlot(fi.src0Slot) && validSlot(fi.dstSlot) &&
               validSlot(fi.aux0Slot) && validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validDoubleArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalDoubleConstStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.storeSlot) &&
               validDoubleArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalDoubleLocalStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) &&
               validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validDoubleArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalDoubleLocalStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validDoubleArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatLocalStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux0Slot) &&
               validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatLocalStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatReg:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux1Slot) &&
               validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatRegElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.aux1Slot) && validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatRegStore:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.dstSlot) && validSlot(fi.aux1Slot) && validSlot(fi.storeSlot) &&
               validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryLocalFloatRegStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validFloatArithOp(fi.binaryOpcode1);
    case FO::SuperBinaryRegFloatStoreElided:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               validSlot(fi.storeSlot) && validFloatArithOp(fi.binaryOpcode1);
    case FO::LoadElementIntFast:
    case FO::LoadElementTypedFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::StoreElementIntFast:
    case FO::StoreElementPrimFast:
    case FO::StoreElementStructFast:
    case FO::StoreElementObjFast:
    case FO::StoreElementStructFieldInPlace:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot) && validSlot(fi.src2Slot);
    case FO::InitStructSlice:
    case FO::InitStackRefCell:
    case FO::BindLocalStackRef:
        return validSlot(fi.dstSlot) && fi.auxImm32 < execFunc.totalFrameSlots;
    case FO::AliasStructSlice:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::CopyStructSlice:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && fi.auxImm32 < execFunc.totalFrameSlots;
    case FO::LoadStructSliceField:
    case FO::LoadStackRefCell:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) &&
               (fi.auxImm32 == runtime::detail::InvalidStreamOffset || fi.auxImm32 < execFunc.totalFrameSlots);
    case FO::StoreStructSliceField:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot) &&
               fi.auxImm32 < execFunc.totalFrameSlots;
    case FO::StoreLocalStructFieldInPlace:
        return validSlot(fi.src1Slot) && (fi.auxImm32 + fi.aux0Slot) < execFunc.totalFrameSlots;
    case FO::WritebackLocalStackRef:
        return validSlot(fi.src0Slot) && fi.auxImm32 < execFunc.totalFrameSlots;
    case FO::StoreStackRefCell:
    case FO::StoreStackRefStructFieldInPlace:
    case FO::StoreFieldExactFast:
    case FO::StoreFieldFast:
        return validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::LoadFieldExactFast:
    case FO::LoadFieldFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::NewObjectFast:
        return validSlot(fi.dstSlot);
    case FO::IsTypeObjFast:
    case FO::AsTypeObjFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot);
    case FO::NewDelegateFast:
        return validSlot(fi.dstSlot) &&
               (fi.src0Slot == runtime::detail::InvalidCompactReg || validSlot(fi.src0Slot));
    case FO::CombineDelegateFast:
    case FO::RemoveDelegateFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::InvokeDelegateFast: {
        if (!validSlot(fi.src0Slot)) return false;
        if (fi.dstSlot != runtime::detail::InvalidCompactReg && !validSlot(fi.dstSlot)) {
            return false;
        }
        if (fi.inlineArgCount >= 1u) {
            return (fi.inlineArgCount < 2u || validSlot(fi.src1Slot)) &&
                   (fi.inlineArgCount < 3u || validSlot(fi.src2Slot));
        }
        const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
        if (idx >= execFunc.flatCold.size()) return false;
        const auto cold = execFunc.resolveCold(idx);
        if (!cold.bytecode || cold.bytecode->operands.empty()) return false;
        for (const auto opReg : cold.bytecode->operands) {
            const auto slot = static_cast<std::size_t>(execFunc.registerOffset) + opReg;
            if (slot >= execFunc.totalFrameSlots) return false;
        }
        return true;
    }
    case FO::GenericBytecode: {
        const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
        const auto cold = execFunc.resolveCold(idx);
        if (idx >= execFunc.flatCold.size() ||
            cold.inlinedCallee != nullptr ||
            cold.bytecode == nullptr ||
            execFunc.bytecode == nullptr) {
            return false;
        }
        const auto& bc = *cold.bytecode;
        const auto& bcFn = *execFunc.bytecode;
        const auto regCnt = bcFn.registerTypes.size();
        const auto regOff = execFunc.registerOffset;
        switch (bc.opcode) {
        case bytecode::Opcode::ConstantInt: {
            if (bc.result >= regCnt || !validSlot(static_cast<std::uint16_t>(regOff + bc.result))) return false;
            const auto rt = bcFn.registerTypes[bc.result];
            return rt == semantic::PrimitiveType::Int ||
                   rt == semantic::PrimitiveType::Long ||
                   rt == semantic::PrimitiveType::Enum;
        }
        case bytecode::Opcode::ConstantNull:
        case bytecode::Opcode::ConstantString:
            return bc.result < regCnt && validSlot(static_cast<std::uint16_t>(regOff + bc.result));
        case bytecode::Opcode::ConvertNullToString:
        case bytecode::Opcode::ConvertNullToObject:
        case bytecode::Opcode::ConvertNullToArray:
            return bc.result < regCnt &&
                   bc.operands.size() == 1 &&
                   bc.operands[0] < regCnt &&
                   validSlot(static_cast<std::uint16_t>(regOff + bc.result)) &&
                   validSlot(static_cast<std::uint16_t>(regOff + bc.operands[0]));
        case bytecode::Opcode::Equal:
        case bytecode::Opcode::NotEqual: {
            if (bc.result >= regCnt || bc.operands.size() != 2 ||
                bc.operands[0] >= regCnt || bc.operands[1] >= regCnt) {
                return false;
            }
            if (!validSlot(static_cast<std::uint16_t>(regOff + bc.result)) ||
                !validSlot(static_cast<std::uint16_t>(regOff + bc.operands[0])) ||
                !validSlot(static_cast<std::uint16_t>(regOff + bc.operands[1]))) {
                return false;
            }
            const auto t0 = bcFn.registerTypes[bc.operands[0]];
            const auto t1 = bcFn.registerTypes[bc.operands[1]];
            const auto okEqType = [](semantic::PrimitiveType t) noexcept {
                return t == semantic::PrimitiveType::Int ||
                       t == semantic::PrimitiveType::UInt ||
                       t == semantic::PrimitiveType::Long ||
                       t == semantic::PrimitiveType::ULong ||
                       t == semantic::PrimitiveType::Byte ||
                       t == semantic::PrimitiveType::SByte ||
                       t == semantic::PrimitiveType::Short ||
                       t == semantic::PrimitiveType::UShort ||
                       t == semantic::PrimitiveType::Char ||
                       t == semantic::PrimitiveType::Double ||
                       t == semantic::PrimitiveType::Float ||
                       t == semantic::PrimitiveType::Bool ||
                       t == semantic::PrimitiveType::Enum ||
                       t == semantic::PrimitiveType::Struct ||
                       t == semantic::PrimitiveType::Object ||
                       t == semantic::PrimitiveType::Array ||
                       t == semantic::PrimitiveType::String ||
                       t == semantic::PrimitiveType::Null;
            };
            return okEqType(t0) && okEqType(t1);
        }
        default:
            return false;
        }
    }
    case FO::ConstantNullTagFast:
        return validSlot(fi.dstSlot) &&
               (fi.aux0Slot == 0u || (fi.aux0Slot <= 3u && validSlot(fi.src0Slot)));
    case FO::ConstantEnumFast:
    case FO::ConstantStringFast:
        return validSlot(fi.dstSlot);
    case FO::EqualValueFast:
        return validSlot(fi.dstSlot) && validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
    case FO::CallIntrinsicFast: {
        if (fi.inlineArgCount > 3u) return false;
        if (fi.dstSlot != runtime::detail::InvalidCompactReg && !validSlot(fi.dstSlot)) {
            return false;
        }
        if (fi.inlineArgCount >= 1u && !validSlot(fi.src0Slot)) return false;
        if (fi.inlineArgCount >= 2u && !validSlot(fi.src1Slot)) return false;
        if (fi.inlineArgCount >= 3u && !validSlot(fi.src2Slot)) return false;
        const auto ikind = static_cast<runtime::detail::FastIntrinsicKind>(
            static_cast<std::uint8_t>(fi.binaryOpcode1));
        if (ikind == runtime::detail::FastIntrinsicKind::None) return false;
        if (ikind == runtime::detail::FastIntrinsicKind::StaticHas ||
            ikind == runtime::detail::FastIntrinsicKind::StaticGet) {
            return validSlot(fi.dstSlot);
        }
        if (ikind == runtime::detail::FastIntrinsicKind::StaticSet) {
            return fi.inlineArgCount >= 1u;
        }
        if (ikind == runtime::detail::FastIntrinsicKind::ArrayPush ||
            ikind == runtime::detail::FastIntrinsicKind::ArrayRemoveAt) {
            return fi.inlineArgCount == 2u;
        }
        return true;
    }
    case FO::CallDirect0:
    case FO::CallDirect1:
    case FO::CallDirect2:
    case FO::CallDirectN: {
        const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
        if (idx >= execFunc.flatCold.size()) return false;
        if (fi.dstSlot != runtime::detail::InvalidCompactReg && !validSlot(fi.dstSlot)) {
            return false;
        }
        const auto cold = execFunc.resolveCold(idx);
        if (cold.inlinedCallee != nullptr ||
            !cold.directTarget ||
            !cold.directTarget->function) {
            return false;
        }
        if (fi.flatOpcode == FO::CallDirect1) return validSlot(fi.src0Slot);
        if (fi.flatOpcode == FO::CallDirect2) return validSlot(fi.src0Slot) && validSlot(fi.src1Slot);
        if (fi.flatOpcode == FO::CallDirectN) {
            if (!cold.bytecode) return false;
            for (const auto opReg : cold.bytecode->operands) {
                const auto slot = static_cast<std::size_t>(execFunc.registerOffset) + opReg;
                if (slot >= execFunc.totalFrameSlots) return false;
            }
        }
        return true;
    }
    case FO::CallPolledFast:
    case FO::SuperCheckNotNullCallPolledFast: {
        const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
        if (idx >= execFunc.flatCold.size() || !validSlot(fi.src0Slot)) return false;
        if (fi.dstSlot != runtime::detail::InvalidCompactReg && !validSlot(fi.dstSlot)) {
            return false;
        }
        const auto cold = execFunc.resolveCold(idx);
        if (cold.inlinedCallee != nullptr) return false;
        if (fi.inlineArgCount >= 1u && fi.inlineArgCount <= 3u) {
            return (fi.inlineArgCount < 2u || validSlot(fi.src1Slot)) &&
                   (fi.inlineArgCount < 3u || validSlot(fi.src2Slot));
        }
        if (!cold.bytecode || cold.bytecode->operands.empty()) return false;
        for (const auto opReg : cold.bytecode->operands) {
            const auto slot = static_cast<std::size_t>(execFunc.registerOffset) + opReg;
            if (slot >= execFunc.totalFrameSlots) return false;
        }
        return true;
    }
    case FO::InlineCallEnter:
        return validSlot(fi.dstSlot) && fi.inlineArgCount <= 4u;
    case FO::InlineReturnVoid:
    case FO::InlineReturnVoidJump:
    case FO::InlineReturnScalar:
    case FO::InlineReturnScalarJump: {
        if (fi.flatOpcode == FO::InlineReturnVoidJump &&
            fi.trueTargetOffset >= execFunc.flatStream.size()) {
            return false;
        }
        if (fi.flatOpcode == FO::InlineReturnScalar ||
            fi.flatOpcode == FO::InlineReturnScalarJump) {
            if (!validSlot(fi.src0Slot) ||
                (fi.dstSlot != runtime::detail::InvalidCompactReg && !validSlot(fi.dstSlot))) {
                return false;
            }
            if (fi.flatOpcode == FO::InlineReturnScalarJump &&
                fi.trueTargetOffset >= execFunc.flatStream.size()) {
                return false;
            }
        }
        if (fi.integerImmediate == 0) return true;
        const auto packed = static_cast<std::uint64_t>(fi.integerImmediate);
        const auto clearBase = static_cast<std::uint32_t>(packed & 0xFFFFu);
        const auto clearCount = static_cast<std::uint32_t>(packed >> 16u);
        return clearBase <= execFunc.totalFrameSlots &&
               clearCount <= execFunc.totalFrameSlots - clearBase &&
               clearCount <= 256u;
    }
    case FO::TermJumpNoArgs:
        return fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::TermBranchBoolNoArgs:
        return validSlot(fi.src0Slot) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::TermBranchGeneric: {
        if (fi.trueTargetOffset >= execFunc.flatStream.size() ||
            (fi.falseTargetOffset != runtime::detail::InvalidStreamOffset &&
             fi.falseTargetOffset >= execFunc.flatStream.size())) {
            return false;
        }
        const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
        if (idx >= execFunc.flatCold.size()) return false;
        const auto cold = execFunc.resolveCold(idx);
        if (cold.blockIndex >= execFunc.blocks.size()) return false;
        const auto& blk = execFunc.blocks[cold.blockIndex];
        const auto validEdge = [&](const runtime::detail::ExecutionEdge& edge) noexcept -> bool {
            if (!edge.hasArguments) return true;
            if (!edge.sources || !edge.destinations) return false;
            const auto& srcs = *edge.sources;
            const auto& dsts = *edge.destinations;
            if (srcs.size() != dsts.size() || srcs.size() > execFunc.edgeScratchCount) {
                return false;
            }
            for (std::size_t k = 0; k < srcs.size(); ++k) {
                const std::size_t srcSlot = static_cast<std::size_t>(execFunc.registerOffset) + srcs[k];
                const std::size_t dstSlot = static_cast<std::size_t>(execFunc.registerOffset) + dsts[k].target;
                if (srcSlot >= execFunc.totalFrameSlots || dstSlot >= execFunc.totalFrameSlots) {
                    return false;
                }
                if (execFunc.hasStructPayloads &&
                    dstSlot < execFunc.slotStructSchemaIds.size() &&
                    execFunc.slotStructSchemaIds[dstSlot] != 0u) {
                    return false;
                }
            }
            return true;
        };
        if (!validEdge(blk.branch)) return false;
        if (fi.falseTargetOffset != runtime::detail::InvalidStreamOffset &&
            !validEdge(blk.falseBranch)) {
            return false;
        }
        return true;
    }
    case FO::TermReturnVoid:
        return true;
    case FO::TermReturnScalarFast:
        return validSlot(fi.src0Slot);
    case FO::TermReturnGeneric:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool isFunctionEligibleForStencilJit(
    const runtime::detail::ExecutionFunction& execFunc) noexcept {
    if (!execFunc.canUseFlatFastPath ||
        execFunc.flatStream.empty() ||
        execFunc.hasExceptionHandlers ||
        execFunc.isAsyncFunction ||
        execFunc.directNativeReference != nullptr ||
        execFunc.bytecode == nullptr ||
        execFunc.entryStreamOffset >= execFunc.flatStream.size() ||
        execFunc.totalFrameSlots == 0 ||
        execFunc.totalFrameSlots > 4096) {
        return false;
    }
    for (const auto& fi : execFunc.flatStream) {
        if (!isStencilSupportedFlatOp(execFunc, fi)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string explainStencilJitIneligibility(
    const runtime::detail::ExecutionFunction& execFunc) {
    if (!execFunc.bytecode) {
        return "missing bytecode body";
    }
    if (execFunc.hasExceptionHandlers || !execFunc.bytecode->exceptionHandlers.empty()) {
        return "function contains try/catch exception handlers";
    }
    if (execFunc.isAsyncFunction) {
        return "async state-machine function runs via interpreter";
    }
    if (execFunc.directNativeReference != nullptr) {
        return "direct host-native reference";
    }
    if (!execFunc.canUseFlatFastPath) {
        return "flat fast-path disabled for function";
    }
    if (execFunc.flatStream.empty()) {
        return "empty flat instruction stream";
    }
    if (execFunc.entryStreamOffset >= execFunc.flatStream.size()) {
        return "invalid entry stream offset";
    }
    if (execFunc.totalFrameSlots == 0 || execFunc.totalFrameSlots > 4096) {
        return "frame slot count outside StencilJit range (1..4096)";
    }
    for (std::size_t i = 0; i < execFunc.flatStream.size(); ++i) {
        const auto& fi = execFunc.flatStream[i];
        if (isStencilSupportedFlatOp(execFunc, fi)) {
            continue;
        }
        if (fi.flatOpcode == runtime::detail::FlatOpcode::GenericBytecode &&
            i < execFunc.flatCold.size()) {
            const auto cold = execFunc.resolveCold(i);
            if (cold.inlinedCallee != nullptr) {
                return "unsupported inlined call inside GenericBytecode";
            }
            if (cold.bytecode != nullptr) {
                return std::string("unsupported generic bytecode opcode: ") +
                    bytecode::opcodeName(cold.bytecode->opcode);
            }
        }
        return "unsupported flat opcode #" +
            std::to_string(static_cast<unsigned>(fi.flatOpcode));
    }
    return {};
}

// ============================================================================

} // namespace jit_detail
} // namespace realscript::jit
