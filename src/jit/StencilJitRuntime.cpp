#include "JitInternal.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
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
#if defined(_WIN32)
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
#if defined(_WIN32)
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
#if defined(_WIN32)
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
#if defined(_WIN32)
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
    for (auto& blk : blocks_) {
        if (blk.executable) continue;
        if (blk.used < blk.capacity) {
            ScopedJitWriteEnable writeGuard(blk.mapJit);
            fillArchTrapPadding(blk.base + blk.used, blk.capacity - blk.used);
        }
        if (!osProtectRxPages(blk.base, blk.capacity, blk.mapJit)) {
            return false;
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
#if defined(__x86_64__) || defined(_M_X64)
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
#elif defined(__aarch64__) || defined(_M_ARM64)
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
        const auto& cold = execFunc.flatCold[flatIdx];
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

// Slow-path C++ helper for NewArrayFast
std::uint32_t jitNewArrayHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    const auto& cold = execFunc.flatCold[flatIdx];
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

    bool slotsMoved = false;
    runtime::RuntimeError allocErr;
    const auto ref = session.heap->allocateTypedArray(
        arrayTid,
        elemType,
        elemTid,
        static_cast<std::size_t>(lenVal),
        initVal,
        &allocErr,
        &slotsMoved);
    if (!ref.has_value()) {
        ctx->remainingBudget -= opDoneCost;
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
    if (slotsMoved) {
        invalidateJitArrayCaches(*ctx);
    }
    if (session.limits.gcWorkBudget != 0 &&
        (session.heap->phase() != runtime::GcPhase::Idle || session.heap->collectionRequested())) {
        const auto work = session.heap->step(session.shadowStack, session.limits.gcWorkBudget);
        session.stats.gcWorkPerformed += work;
        invalidateJitArrayCaches(*ctx);
    }
    ctx->frameBase[fi.dstSlot] = *ref;
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
    if (bindJitArrayCacheSlot(session.heap, arrVal, ce)) {
        len64 = static_cast<std::int64_t>(ce.length);
    } else if (const auto* ref = std::get_if<runtime::ObjectRef>(&arrVal); ref && session.heap) {
        if (!session.heap->isAlive(*ref)) {
            if (ctx->remainingBudget < opDoneCost) {
                ctx->remainingBudget = 0;
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
            }
            ctx->remainingBudget -= opDoneCost;
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid array reference");
        }
        if (const auto optLen = session.heap->arrayLength(*ref)) {
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
    const auto& cold = execFunc.flatCold[flatIdx];
    auto& session = *ctx->session;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    const runtime::Value& arrVal = ctx->frameBase[fi.src0Slot];
    const runtime::Value& idxVal = ctx->frameBase[fi.src1Slot];

    // Lock-free JitArrayCacheEntry fast path for FlatStruct[] & BlittableStruct[]!
    if (fi.flatOpcode == runtime::detail::FlatOpcode::LoadElementTypedFast &&
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
    if (!session.heap->isAlive(ref)) {
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

    auto& ce = ctx->arrayCache[fi.src0Slot & 3u];
    (void)bindJitArrayCacheSlot(session.heap, arrVal, ce);

    const auto lengthOpt = session.heap->arrayLength(ref);
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

    const auto actualElemType = session.heap->arrayElementType(ref);
    const auto actualElemTid = session.heap->arrayElementTypeId(ref);
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
            if ((session.heap->tryBindFlatStructArrayFastView(ref, fView) && fView.elementTypeId == expectedTid && fView.flatStride == sc->flatSlotCount) &&
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
            if (session.heap->tryBindPackedArrayFastView(ref, pView) &&
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

    const auto loaded = session.heap->arrayGet(ref, static_cast<std::size_t>(idx));
    if (!loaded) {
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "array element load failed");
    }

    ctx->remainingBudget -= fi.span;
    if (execFunc.hasStructPayloads &&
        session.flatBankId != 0u &&
        fi.dstSlot < execFunc.slotStructSchemaIds.size() &&
        execFunc.slotStructSchemaIds[fi.dstSlot] != 0u) {
        const std::uint16_t sid = execFunc.slotStructSchemaIds[fi.dstSlot];
        const std::uint16_t payOff = execFunc.slotPayloadOffsets[fi.dstSlot];
        const auto* sc = runtime::getFlatStructSchema(sid);
        if (sc != nullptr && payOff != runtime::detail::InvalidCompactReg &&
            runtime::extractStructLeavesToBuffer(*loaded, *sc, ctx->frameBase + payOff)) {
            ctx->frameBase[fi.dstSlot] = runtime::StructValue::makeFrameSlice(
                sc->structTypeId,
                session.flatBankId,
                static_cast<std::uint32_t>(session.frameOffset + payOff),
                sid);
            return 0u;
        }
    }
    ctx->frameBase[fi.dstSlot] = *loaded;
    return 0u;
}

// Slow-path C++ helper for StoreElementIntFast & StoreElementPrimFast
std::uint32_t jitStoreElementHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    const auto& cold = execFunc.flatCold[flatIdx];
    auto& session = *ctx->session;

    const std::uint8_t unexecAfter = ctx->compiledFn->unexecutedAfterOp[flatIdx];
    const std::uint64_t opDoneCost = fi.span - unexecAfter;

    const runtime::Value& arrVal = ctx->frameBase[fi.src0Slot];
    const runtime::Value& idxVal = ctx->frameBase[fi.src1Slot];
    const runtime::Value& storeVal = ctx->frameBase[fi.src2Slot];

    // Lock-free JitArrayCacheEntry fast path for FlatStruct[] & BlittableStruct[] stores!
    if ((fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFieldInPlace ||
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
    if (!session.heap->isAlive(ref)) {
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

    const auto lengthOpt = session.heap->arrayLength(ref);
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
            session.heap->tryBindPackedArrayFastView(ref, pView) &&
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
            if (auto elemOpt = session.heap->arrayGet(ref, uIdx); elemOpt.has_value()) {
                std::vector<runtime::Value> tmpLeaves(rootSc->flatSlotCount);
                if (runtime::extractStructLeavesToBuffer(*elemOpt, *rootSc, tmpLeaves.data())) {
                    if (fi.aux1Slot == 0u) {
                        tmpLeaves[fi.aux0Slot] = runtime::detachFrameSliceValue(storeVal);
                    } else if (const auto* subSc = runtime::getFlatStructSchema(fi.aux1Slot)) {
                        (void)runtime::extractStructLeavesToBuffer(storeVal, *subSc, tmpLeaves.data() + fi.aux0Slot);
                    }
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(*rootSc, tmpLeaves.data());
                    runtime::RuntimeError demErr;
                    if (session.heap->arraySet(ref, uIdx, rebuilt, &demErr)) {
                        ctx->remainingBudget -= fi.span;
                        invalidateJitArrayCaches(*ctx);
                        (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
                        return 0u;
                    }
                }
            }
        }
        runtime::FlatStructArrayFastView fView{};
        if (rootSc != nullptr &&
            (session.heap->tryBindFlatStructArrayFastView(ref, fView) && fView.elementTypeId == expectedTid && fView.flatStride == rootSc->flatSlotCount) &&
            fView.flatSchemaId == rootSid &&
            fi.aux0Slot < fView.flatStride) {
            runtime::Value* dstLeaf = fView.data + uIdx * fView.flatStride + fi.aux0Slot;
            if (fi.aux1Slot == 0u) {
                *dstLeaf = runtime::detachFrameSliceValue(storeVal);
                if (fView.containsManagedReferences) {
                    if (fView.dirtyCards != nullptr) {
                        fView.dirtyCards[uIdx >> runtime::ManagedHeap::kCardElementShift] = 1u;
                    }
                    if (fView.needsRememberedBarrier || session.heap->phase() != runtime::GcPhase::Idle) {
                        session.heap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                    }
                }
                // If containsPoolValues, delegate via arraySet only when value is pool-backed String/Arena
                if (fView.containsPoolValues &&
                    (dstLeaf->rawTag() == runtime::ValueTag::String ||
                     dstLeaf->rawTag() == runtime::ValueTag::QuatArena ||
                     dstLeaf->rawTag() == runtime::ValueTag::QuatfArena)) {
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(
                        *rootSc, fView.data + uIdx * fView.flatStride);
                    (void)session.heap->arraySet(ref, uIdx, rebuilt, nullptr);
                    invalidateJitArrayCaches(*ctx);
                    (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
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
                    if (fView.needsRememberedBarrier || session.heap->phase() != runtime::GcPhase::Idle) {
                        session.heap->recordFlatStructArrayWriteBarrier(ref, uIdx);
                    }
                }
                if (fView.containsPoolValues) {
                    const runtime::Value rebuilt = runtime::reconstructDetachedStructFromLeaves(
                        *rootSc, fView.data + uIdx * fView.flatStride);
                    (void)session.heap->arraySet(ref, uIdx, rebuilt, nullptr);
                    invalidateJitArrayCaches(*ctx);
                    (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
                }
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
        }
        ctx->remainingBudget -= opDoneCost;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "in-place struct element field store failed");
    }

    semantic::PrimitiveType expectedType = semantic::PrimitiveType::Int;
    if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementPrimFast ||
        fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementObjFast) {
        expectedType = static_cast<semantic::PrimitiveType>(static_cast<std::uint8_t>(fi.binaryOpcode1));
    } else if (fi.flatOpcode == runtime::detail::FlatOpcode::StoreElementStructFast) {
        expectedType = semantic::PrimitiveType::Struct;
    } else if (cold.bytecode) {
        expectedType = cold.bytecode->elementType;
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

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }

    // Note: ManagedHeap::arraySet already handles FrameSlice StructValue with ZERO StructStorage allocations!
    runtime::RuntimeError setErr;
    if (!session.heap->arraySet(ref, static_cast<std::size_t>(idx), storeVal, &setErr)) {
        ctx->remainingBudget -= opDoneCost;
        const auto code = setErr.code == runtime::ErrorCode::None
            ? runtime::ErrorCode::TypeMismatch
            : setErr.code;
        return setHelperError(ctx, flatIdx, code, setErr.message.empty() ? "arraySet failed" : setErr.message);
    }

    ctx->remainingBudget -= fi.span;
    invalidateJitArrayCaches(*ctx);
    // Re-bind cache slot in case PackedI32 widened to PackedI64 or GC ran
    (void)bindJitArrayCacheSlot(session.heap, arrVal, ctx->arrayCache[fi.src0Slot & 3u]);
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

// Fast C++ runtime helper for FlatStruct slice & StackSlotRef opcodes in StencilJit
std::uint32_t jitFlatStructAndStackRefHelper(JitAbiContext* ctx, std::uint32_t flatIdx) noexcept {
    const auto& execFunc = *ctx->compiledFn->execFunc;
    const auto& fi = execFunc.flatStream[flatIdx];
    auto& session = *ctx->session;
    using FO = runtime::detail::FlatOpcode;

    if (ctx->remainingBudget < fi.span) {
        ctx->remainingBudget = 0;
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InstructionBudgetExceeded, "instruction budget exceeded");
    }
    if (fi.flatOpcode == FO::CheckNotNullFast) {
        const runtime::Value& val = ctx->frameBase[fi.src0Slot];
        if (val.rawTag() == runtime::ValueTag::ObjectRef) {
            const runtime::ObjectRef ref = val.u_.objectRef;
            if ((ref.isStackSlotRef() && ref.generation == session.flatBankId) ||
                (session.heap != nullptr && session.heap->isAlive(ref))) {
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = val;
                return 0u;
            }
        }
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::NullReference, "null object reference");
    }
    if (fi.flatOpcode == FO::NewObjectFast) {
        if (!session.heap || !session.program) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "object allocation requires heap");
        }
        const auto& mirror = runtime::detail::executionMirror(*session.program);
        if (fi.aux0Slot >= mirror.objectTemplates.size()) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "invalid object template");
        }
        const auto& tmpl = mirror.objectTemplates[fi.aux0Slot];
        runtime::RuntimeError allocErr;
        const auto allocated = session.heap->allocateObjectFromTemplate(
            tmpl.typeId,
            tmpl.defaultFields.data(),
            tmpl.defaultFields.size(),
            tmpl.referenceFields,
            &allocErr);
        if (!allocated) {
            return setHelperError(
                ctx,
                flatIdx,
                allocErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::OutOfMemory : allocErr.code,
                allocErr.message.empty() ? "managed object allocation failed" : allocErr.message);
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = *allocated;
        invalidateJitArrayCaches(*ctx);
        return 0u;
    }
    if (fi.flatOpcode == FO::LoadFieldExactFast || fi.flatOpcode == FO::LoadFieldFast) {
        const runtime::Value& recvVal = ctx->frameBase[fi.src0Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            const auto errCode = (recvVal.rawTag() == runtime::ValueTag::NullObject)
                ? runtime::ErrorCode::NullReference
                : runtime::ErrorCode::TypeMismatch;
            return setHelperError(ctx, flatIdx, errCode, "field load attempted on null or non-object");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        if (ref.isStackSlotRef()) {
            if (ref.generation != session.flatBankId || !session.values) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid stack ref");
            }
            if (ref.stackRefSchemaId() == 0u) {
                ctx->remainingBudget -= fi.span;
                ctx->frameBase[fi.dstSlot] = (*session.values)[ref.slot];
                return 0u;
            }
        }
        if (!session.heap) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "field load missing heap");
        }
        runtime::ObjectFastView view{};
        if (!session.heap->tryBindObjectFastView(ref, view)) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "dangling object reference");
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
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "field load receiver type mismatch");
        }
        ctx->remainingBudget -= fi.span;
        ctx->frameBase[fi.dstSlot] = view.fields[fi.aux0Slot];
        return 0u;
    }
    if (fi.flatOpcode == FO::StoreFieldExactFast || fi.flatOpcode == FO::StoreFieldFast) {
        const runtime::Value& recvVal = ctx->frameBase[fi.src0Slot];
        const runtime::Value& storeVal = ctx->frameBase[fi.src1Slot];
        if (recvVal.rawTag() != runtime::ValueTag::ObjectRef) {
            const auto errCode = (recvVal.rawTag() == runtime::ValueTag::NullObject)
                ? runtime::ErrorCode::NullReference
                : runtime::ErrorCode::TypeMismatch;
            return setHelperError(ctx, flatIdx, errCode, "field store attempted on null or non-object");
        }
        const runtime::ObjectRef ref = recvVal.u_.objectRef;
        if (ref.isStackSlotRef()) {
            if (ref.generation != session.flatBankId || !session.values) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "invalid stack ref");
            }
            if (ref.stackRefSchemaId() == 0u) {
                (*session.values)[ref.slot] = runtime::detachFrameSliceValue(storeVal);
                ctx->remainingBudget -= fi.span;
                return 0u;
            }
        }
        if (!session.heap) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "field store missing heap");
        }
        const auto actualTid = session.heap->objectTypeId(ref);
        if (!actualTid) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidObjectReference, "dangling object reference");
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
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "field store receiver type mismatch");
        }
        runtime::RuntimeError setErr;
        if (!session.heap->fieldSet(ref, fi.aux0Slot, runtime::detachFrameSliceValue(storeVal), &setErr)) {
            return setHelperError(
                ctx,
                flatIdx,
                setErr.code == runtime::ErrorCode::None ? runtime::ErrorCode::InvalidProgram : setErr.code,
                setErr.message.empty() ? "field store failed" : setErr.message);
        }
        ctx->remainingBudget -= fi.span;
        return 0u;
    }
    if (fi.flatOpcode == FO::GenericBytecode) {
        if (flatIdx >= execFunc.flatCold.size() ||
            execFunc.flatCold[flatIdx].bytecode == nullptr ||
            execFunc.bytecode == nullptr) {
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "missing GenericBytecode metadata");
        }
        const auto& bc = *execFunc.flatCold[flatIdx].bytecode;
        const auto& bcFn = *execFunc.bytecode;
        const auto regOff = execFunc.registerOffset;
        switch (bc.opcode) {
        case bytecode::Opcode::ConstantInt: {
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
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::TypeMismatch, "expected null for conversion");
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
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "ConstantString requires heap");
            }
            runtime::RuntimeError strErr;
            const auto interned = session.heap->internString(bc.stringImmediate, &strErr);
            if (!interned) {
                return setHelperError(ctx, flatIdx, runtime::ErrorCode::OutOfMemory, strErr.message);
            }
            ctx->frameBase[regOff + bc.result] = *interned;
            ctx->remainingBudget -= fi.span;
            invalidateJitArrayCaches(*ctx);
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
                return setHelperError(ctx, flatIdx, eqErrCode, eqErrMsg);
            }
            ctx->frameBase[regOff + bc.result] = (bc.opcode == bytecode::Opcode::Equal) ? eq : !eq;
            ctx->remainingBudget -= fi.span;
            return 0u;
        }
        default:
            return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "unsupported GenericBytecode in StencilJit");
        }
    }
    if (session.flatBankId == 0u || session.values == nullptr) {
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "flat stack bank is inactive in StencilJit");
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
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::InvalidProgram, "unsupported flat struct/ref op in StencilJit");
    }
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
    ctx->remainingBudget -= 1u;
    if (session.callDepth >= session.limits.recursionLimit) {
        return setHelperError(ctx, flatIdx, runtime::ErrorCode::RecursionLimitExceeded, "recursion limit exceeded");
    }
    ++session.stats.functionCalls;
    if (session.callDepth + 1 > session.stats.maximumCallDepth) {
        session.stats.maximumCallDepth = session.callDepth + 1;
    }

    runtime::Value* const calleeBase = ctx->frameBase + fi.dstSlot;
    const std::uint32_t nArgs = fi.inlineArgCount;
    const std::uint32_t nLocals = fi.aux0Slot;
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
    const auto& cold = execFunc.flatCold[flatIdx];
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
        const std::size_t start = cold.instructionIndexInBlock;
        const std::size_t limit = std::min<std::size_t>(blk.instructions.size(), start + fi.span);
        for (std::size_t m = start; m < limit; ++m) {
            if (blk.instructions[m].bytecode == cold.bytecode) {
                const std::size_t done = (m - start) + 1u;
                if (done <= fi.span) {
                    return static_cast<std::uint8_t>(fi.span - done);
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
    case FO::NegateIntUnchecked:
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
            if (idx >= execFunc.flatCold.size() || execFunc.flatCold[idx].bytecode == nullptr) {
                return false;
            }
            const auto elemTid = execFunc.flatCold[idx].bytecode->elementTypeId;
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
    case FO::GenericBytecode: {
        const auto idx = static_cast<std::size_t>(&fi - execFunc.flatStream.data());
        if (idx >= execFunc.flatCold.size() ||
            execFunc.flatCold[idx].inlinedCallee != nullptr ||
            execFunc.flatCold[idx].bytecode == nullptr ||
            execFunc.bytecode == nullptr) {
            return false;
        }
        const auto& bc = *execFunc.flatCold[idx].bytecode;
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
    case FO::InlineCallEnter:
        return validSlot(fi.dstSlot) && fi.inlineArgCount <= 4u;
    case FO::InlineReturnVoid:
        return true;
    case FO::InlineReturnVoidJump:
        return fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::InlineReturnScalar:
        return validSlot(fi.src0Slot) &&
               (fi.dstSlot == runtime::detail::InvalidCompactReg || validSlot(fi.dstSlot));
    case FO::InlineReturnScalarJump:
        return validSlot(fi.src0Slot) &&
               (fi.dstSlot == runtime::detail::InvalidCompactReg || validSlot(fi.dstSlot)) &&
               fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::TermJumpNoArgs:
        return fi.trueTargetOffset < execFunc.flatStream.size();
    case FO::TermBranchBoolNoArgs:
        return validSlot(fi.src0Slot) &&
               fi.trueTargetOffset < execFunc.flatStream.size() &&
               fi.falseTargetOffset < execFunc.flatStream.size();
    case FO::TermBranchGeneric:
        return fi.trueTargetOffset < execFunc.flatStream.size() &&
               (fi.falseTargetOffset == runtime::detail::InvalidStreamOffset ||
                fi.falseTargetOffset < execFunc.flatStream.size());
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

// ============================================================================

} // namespace jit_detail
} // namespace realscript::jit
