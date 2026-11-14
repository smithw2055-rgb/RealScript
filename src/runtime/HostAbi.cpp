#include "realscript/HostAbi.h"

#include "realscript/bytecode/Bytecode.h"
#include "realscript/runtime/Runtime.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

struct RsHeapOpaque {
    std::shared_ptr<realscript::runtime::ManagedHeap> heap;
};

struct RsProgramOpaque {
    std::shared_ptr<const realscript::runtime::ProgramImage> program;
};

struct RsVmOpaque {
    std::shared_ptr<const realscript::runtime::ProgramImage> program;
    std::shared_ptr<realscript::runtime::ManagedHeap> heap;
    std::shared_ptr<realscript::runtime::BindingRegistry> bindings;
    std::unique_ptr<realscript::runtime::Interpreter> interpreter;
};

namespace {

using namespace realscript;
using namespace realscript::runtime;

RsObjectRefV1 toAbiObjectRef(ObjectRef ref) noexcept {
    RsObjectRefV1 out{};
    out.slot = ref.slot;
    out.generation = ref.generation;
    out.heap_id = ref.heapId;
    out.object_kind = static_cast<uint8_t>(ref.kind);
    return out;
}

ObjectRef fromAbiObjectRef(RsObjectRefV1 ref) noexcept {
    ObjectRef out{};
    out.kind = static_cast<ObjectKind>(ref.object_kind);
    out.slot = ref.slot;
    out.generation = ref.generation;
    out.heapId = ref.heap_id;
    return out;
}

RsNativeHandleV1 toAbiNativeHandle(NativeHandle h) noexcept {
    RsNativeHandleV1 out{};
    out.slot = h.slot;
    out.generation = h.generation;
    out.type_id = h.typeId;
    out.registry_id = h.registryId;
    return out;
}

NativeHandle fromAbiNativeHandle(RsNativeHandleV1 h) noexcept {
    return NativeHandle{h.slot, h.generation, h.type_id, h.registry_id};
}

RsStatusV1 marshalValueToAbi(
    ManagedHeap* targetHeapForTempStrings,
    const Value& v,
    RsValueV1* out) noexcept {
    if (!out) return RS_STATUS_V1_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    if (std::holds_alternative<std::monostate>(v)) {
        out->kind = RS_VALUE_KIND_V1_VOID;
        return RS_STATUS_V1_OK;
    }
    if (const auto* b = std::get_if<bool>(&v)) {
        out->kind = RS_VALUE_KIND_V1_BOOL;
        out->payload.bool_val = *b ? 1u : 0u;
        return RS_STATUS_V1_OK;
    }
    if (const auto* i = std::get_if<std::int64_t>(&v)) {
        out->kind = RS_VALUE_KIND_V1_INT64;
        out->payload.i64_val = *i;
        return RS_STATUS_V1_OK;
    }
    if (const auto* u = std::get_if<ULongValue>(&v)) {
        out->kind = RS_VALUE_KIND_V1_UINT64;
        out->payload.u64_val = u->value;
        return RS_STATUS_V1_OK;
    }
    if (const auto* d = std::get_if<double>(&v)) {
        out->kind = RS_VALUE_KIND_V1_DOUBLE;
        out->payload.f64_val = *d;
        return RS_STATUS_V1_OK;
    }
    if (const auto* r = std::get_if<ObjectRef>(&v)) {
        out->kind = RS_VALUE_KIND_V1_OBJECT_REF;
        out->payload.obj_ref = toAbiObjectRef(*r);
        return RS_STATUS_V1_OK;
    }
    if (const auto* s = std::get_if<std::string>(&v)) {
        if (!targetHeapForTempStrings) {
            return RS_STATUS_V1_INVALID_ARGUMENT;
        }
        RuntimeError err;
        const auto allocated = targetHeapForTempStrings->allocateString(*s, &err);
        if (!allocated) return RS_STATUS_V1_OUT_OF_MEMORY;
        out->kind = RS_VALUE_KIND_V1_OBJECT_REF;
        out->payload.obj_ref = toAbiObjectRef(*allocated);
        return RS_STATUS_V1_OK;
    }
    if (const auto* nh = std::get_if<NativeHandle>(&v)) {
        out->kind = RS_VALUE_KIND_V1_NATIVE_HANDLE;
        out->payload.native_handle = toAbiNativeHandle(*nh);
        return RS_STATUS_V1_OK;
    }
    if (std::holds_alternative<NullObject>(v)) {
        out->kind = RS_VALUE_KIND_V1_NULL_OBJECT;
        return RS_STATUS_V1_OK;
    }
    if (std::holds_alternative<NullString>(v)) {
        out->kind = RS_VALUE_KIND_V1_NULL_STRING;
        return RS_STATUS_V1_OK;
    }
    return RS_STATUS_V1_RUNTIME_ERROR;
}

bool marshalValueFromAbi(const RsValueV1& in, Value& out) noexcept {
    switch (in.kind) {
    case RS_VALUE_KIND_V1_VOID:
        out = Value{};
        return true;
    case RS_VALUE_KIND_V1_BOOL:
        out = Value{in.payload.bool_val != 0};
        return true;
    case RS_VALUE_KIND_V1_INT64:
        out = Value{in.payload.i64_val};
        return true;
    case RS_VALUE_KIND_V1_UINT64:
        out = Value{ULongValue{in.payload.u64_val}};
        return true;
    case RS_VALUE_KIND_V1_DOUBLE:
        out = Value{in.payload.f64_val};
        return true;
    case RS_VALUE_KIND_V1_OBJECT_REF:
        out = Value{fromAbiObjectRef(in.payload.obj_ref)};
        return true;
    case RS_VALUE_KIND_V1_NATIVE_HANDLE:
        out = Value{fromAbiNativeHandle(in.payload.native_handle)};
        return true;
    case RS_VALUE_KIND_V1_NULL_OBJECT:
        out = Value{NullObject{}};
        return true;
    case RS_VALUE_KIND_V1_NULL_STRING:
        out = Value{NullString{}};
        return true;
    default:
        return false;
    }
}

RsStatusV1 mapRuntimeError(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None:
        return RS_STATUS_V1_OK;
    case ErrorCode::OutOfMemory:
        return RS_STATUS_V1_OUT_OF_MEMORY;
    case ErrorCode::CrossHeapAccessViolation:
        return RS_STATUS_V1_CROSS_HEAP_VIOLATION;
    case ErrorCode::InvalidObjectReference:
    case ErrorCode::InvalidNativeHandle:
        return RS_STATUS_V1_STALE_REFERENCE;
    case ErrorCode::InvalidArguments:
        return RS_STATUS_V1_INVALID_ARGUMENT;
    default:
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapCreate(size_t maxBytes, RsHeapHandleV1* outHeap) noexcept {
    if (!outHeap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        HeapConfig opts{};
        if (maxBytes > 0) opts.maximumHeapBytes = maxBytes;
        auto* wrapper = new (std::nothrow) RsHeapOpaque{
            std::make_shared<ManagedHeap>(opts)};
        if (!wrapper) return RS_STATUS_V1_OUT_OF_MEMORY;
        *outHeap = wrapper;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

void apiHeapDestroy(RsHeapHandleV1 heap) noexcept {
    delete heap;
}

uint64_t apiHeapId(RsHeapHandleV1 heap) noexcept {
    return (heap && heap->heap) ? heap->heap->heapId() : 0u;
}

RsStatusV1 apiHeapCollect(RsHeapHandleV1 heap) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        ShadowStack emptyRoots;
        heap->heap->collect(emptyRoots);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapCollectBudgetNs(
    RsHeapHandleV1 heap,
    uint64_t budgetNs,
    uint8_t* outCompleted) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        ShadowStack emptyRoots;
        const auto stats = heap->heap->collectIncrementalWithBudget(
            emptyRoots,
            std::chrono::nanoseconds(budgetNs),
            32,
            true);
        if (outCompleted) *outCompleted = stats.cycleCompleted ? 1u : 0u;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapPinObject(
    RsHeapHandleV1 heap,
    RsObjectRefV1 objectRef,
    uint64_t* outRootToken) noexcept {
    if (!heap || !heap->heap || !outRootToken) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        const auto tok = heap->heap->addPersistentRoot(fromAbiObjectRef(objectRef));
        if (tok == 0) return RS_STATUS_V1_STALE_REFERENCE;
        *outRootToken = tok;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

RsStatusV1 apiHeapUnpinRoot(RsHeapHandleV1 heap, uint64_t rootToken) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    return heap->heap->removePersistentRoot(rootToken)
        ? RS_STATUS_V1_OK
        : RS_STATUS_V1_INVALID_ARGUMENT;
}

uint8_t apiHeapIsAlive(RsHeapHandleV1 heap, RsObjectRefV1 objectRef) noexcept {
    if (!heap || !heap->heap) return 0u;
    return heap->heap->isAlive(fromAbiObjectRef(objectRef)) ? 1u : 0u;
}

RsStatusV1 apiHeapSetSweepCallback(
    RsHeapHandleV1 heap,
    RsNativeSweepCallbackV1 callback,
    void* userData) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    try {
        if (!callback) {
            heap->heap->setNativeWrapperSweepCallback(nullptr);
            return RS_STATUS_V1_OK;
        }
        heap->heap->setNativeWrapperSweepCallback(
            [callback, userData](NativeHandle nh, std::uint64_t hid, ObjectRef swept) {
                callback(userData, toAbiNativeHandle(nh), hid, toAbiObjectRef(swept));
            });
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsWeakRefV1 apiWeakFromObject(RsObjectRefV1 objectRef) noexcept {
    RsWeakRefV1 w{};
    w.slot = objectRef.slot;
    w.generation = objectRef.generation;
    w.heap_id = objectRef.heap_id;
    w.object_kind = objectRef.object_kind;
    return w;
}

RsStatusV1 apiWeakTryResolve(
    RsHeapHandleV1 heap,
    RsWeakRefV1 weakRef,
    RsObjectRefV1* outObjectRef) noexcept {
    if (!heap || !heap->heap || !outObjectRef) return RS_STATUS_V1_INVALID_ARGUMENT;
    ObjectRef candidate{};
    candidate.kind = static_cast<ObjectKind>(weakRef.object_kind);
    candidate.slot = weakRef.slot;
    candidate.generation = weakRef.generation;
    candidate.heapId = weakRef.heap_id;
    if (!heap->heap->isAlive(candidate)) {
        return RS_STATUS_V1_STALE_REFERENCE;
    }
    *outObjectRef = toAbiObjectRef(candidate);
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiHeapEnableDirtyTracking(
    RsHeapHandleV1 heap,
    uint64_t typeId,
    const uint8_t* fieldToBitMap,
    uint32_t fieldCount) noexcept {
    if (!heap || !heap->heap || (fieldCount > 0 && !fieldToBitMap)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        std::vector<std::uint8_t> map(fieldToBitMap, fieldToBitMap + fieldCount);
        heap->heap->enableTypeDirtyTracking(typeId, std::move(map));
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiHeapMarkDirtyBit(
    RsHeapHandleV1 heap,
    RsObjectRefV1 objectRef,
    uint8_t bitIndex) noexcept {
    if (!heap || !heap->heap) return RS_STATUS_V1_INVALID_ARGUMENT;
    if (!heap->heap->markDirtyBit(fromAbiObjectRef(objectRef), bitIndex)) {
        return RS_STATUS_V1_STALE_REFERENCE;
    }
    return RS_STATUS_V1_OK;
}

uint64_t apiHeapPeekDirtyMask(RsHeapHandleV1 heap, RsObjectRefV1 objectRef) noexcept {
    if (!heap || !heap->heap) return 0u;
    return heap->heap->peekDirtyMask(fromAbiObjectRef(objectRef));
}

uint64_t apiHeapConsumeDirtyMask(RsHeapHandleV1 heap, RsObjectRefV1 objectRef) noexcept {
    if (!heap || !heap->heap) return 0u;
    return heap->heap->consumeDirtyMask(fromAbiObjectRef(objectRef));
}

RsStatusV1 apiHeapReadFieldWithPolicy(
    RsHeapHandleV1 callerHeap,
    RsHeapHandleV1 targetHeap,
    RsObjectRefV1 targetObject,
    uint32_t fieldIndex,
    RsCrossHeapPolicyV1 policy,
    RsValueV1* outValue) noexcept {
    if (!callerHeap || !callerHeap->heap || !targetHeap || !targetHeap->heap || !outValue) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    RuntimeError err;
    Value readVal{};
    const auto cppPolicy = static_cast<CrossHeapAccessPolicy>(policy);
    if (!callerHeap->heap->readFieldWithPolicy(
            *targetHeap->heap,
            fromAbiObjectRef(targetObject),
            fieldIndex,
            cppPolicy,
            readVal,
            &err)) {
        return mapRuntimeError(err.code);
    }
    return marshalValueToAbi(callerHeap->heap.get(), readVal, outValue);
}

RsStatusV1 apiHeapWriteFieldWithPolicy(
    RsHeapHandleV1 callerHeap,
    RsHeapHandleV1 targetHeap,
    RsObjectRefV1 targetObject,
    uint32_t fieldIndex,
    RsCrossHeapPolicyV1 policy,
    const RsValueV1* newValue) noexcept {
    if (!callerHeap || !callerHeap->heap || !targetHeap || !targetHeap->heap || !newValue) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    Value cppVal{};
    if (!marshalValueFromAbi(*newValue, cppVal)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    RuntimeError err;
    const auto cppPolicy = static_cast<CrossHeapAccessPolicy>(policy);
    if (!callerHeap->heap->writeFieldWithPolicy(
            *targetHeap->heap,
            fromAbiObjectRef(targetObject),
            fieldIndex,
            std::move(cppVal),
            cppPolicy,
            &err)) {
        return mapRuntimeError(err.code);
    }
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiStringAllocUtf8(
    RsHeapHandleV1 heap,
    const char* utf8Data,
    size_t utf8ByteLen,
    RsObjectRefV1* outStringRef) noexcept {
    if (!heap || !heap->heap || !outStringRef || (utf8ByteLen > 0 && !utf8Data)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        RuntimeError err;
        const std::string_view sv(utf8Data ? utf8Data : "", utf8ByteLen);
        const auto ref = heap->heap->allocateString(std::string(sv), &err);
        if (!ref) return mapRuntimeError(err.code);
        *outStringRef = toAbiObjectRef(*ref);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiStringAllocUtf16(
    RsHeapHandleV1 heap,
    const uint16_t* utf16Data,
    size_t utf16UnitLen,
    RsObjectRefV1* outStringRef) noexcept {
    if (!heap || !heap->heap || !outStringRef || (utf16UnitLen > 0 && !utf16Data)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        std::string utf8;
        const std::u16string_view u16(
            reinterpret_cast<const char16_t*>(utf16Data), utf16UnitLen);
        utf16ToUtf8(u16, utf8);
        RuntimeError err;
        const auto ref = heap->heap->allocateString(utf8, &err);
        if (!ref) return mapRuntimeError(err.code);
        *outStringRef = toAbiObjectRef(*ref);
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiStringReadUtf8(
    RsHeapHandleV1 heap,
    RsObjectRefV1 stringRef,
    const char** outUtf8Ptr,
    size_t* outUtf8ByteLen) noexcept {
    if (!heap || !heap->heap || !outUtf8Ptr || !outUtf8ByteLen) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    const auto sv = heap->heap->stringView(fromAbiObjectRef(stringRef));
    if (!sv) return RS_STATUS_V1_STALE_REFERENCE;
    *outUtf8Ptr = sv->data();
    *outUtf8ByteLen = sv->size();
    return RS_STATUS_V1_OK;
}

RsStatusV1 apiStringReadUtf16(
    RsHeapHandleV1 heap,
    RsObjectRefV1 stringRef,
    uint16_t* outUtf16Buf,
    size_t bufCapacityUnits,
    size_t* outRequiredUnits) noexcept {
    if (!heap || !heap->heap || !outRequiredUnits) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    const auto sv = heap->heap->stringView(fromAbiObjectRef(stringRef));
    if (!sv) return RS_STATUS_V1_STALE_REFERENCE;
    try {
        std::u16string u16;
        utf8ToUtf16(*sv, u16);
        *outRequiredUnits = u16.size();
        if (!outUtf16Buf) return RS_STATUS_V1_OK;
        if (bufCapacityUnits < u16.size()) return RS_STATUS_V1_BUFFER_TOO_SMALL;
        if (!u16.empty()) {
            std::memcpy(outUtf16Buf, u16.data(), u16.size() * sizeof(uint16_t));
        }
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiProgramLoadRsbc(
    const uint8_t* rsbcBytes,
    size_t rsbcSize,
    RsProgramHandleV1* outProgram) noexcept {
    if (!rsbcBytes || rsbcSize == 0 || !outProgram) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        std::vector<std::uint8_t> buf(rsbcBytes, rsbcBytes + rsbcSize);
        diagnostics::DiagnosticBag diags;
        bytecode::Module mod;
        if (!bytecode::decodeModule(buf, mod, diags) || diags.hasErrors()) {
            return RS_STATUS_V1_RUNTIME_ERROR;
        }
        std::vector<bytecode::Module> mods;
        mods.push_back(std::move(mod));
        RuntimeError linkErr;
        auto linked = ProgramImage::link(std::move(mods), linkErr);
        if (!linked) return mapRuntimeError(linkErr.code);
        auto* handle = new (std::nothrow) RsProgramOpaque{
            std::make_shared<const ProgramImage>(std::move(*linked))};
        if (!handle) return RS_STATUS_V1_OUT_OF_MEMORY;
        *outProgram = handle;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

void apiProgramDestroy(RsProgramHandleV1 program) noexcept {
    delete program;
}

RsStatusV1 apiVmCreate(
    RsProgramHandleV1 program,
    RsHeapHandleV1 heap,
    RsVmHandleV1* outVm) noexcept {
    if (!program || !program->program || !outVm) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        auto targetHeap = (heap && heap->heap)
            ? heap->heap
            : std::make_shared<ManagedHeap>();
        auto bindings = std::make_shared<BindingRegistry>();
        auto interp = std::make_unique<Interpreter>(
            program->program, targetHeap);
        interp->setBindingRegistry(bindings);
        auto* vm = new (std::nothrow) RsVmOpaque{
            program->program,
            std::move(targetHeap),
            std::move(bindings),
            std::move(interp)};
        if (!vm) return RS_STATUS_V1_OUT_OF_MEMORY;
        *outVm = vm;
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

void apiVmDestroy(RsVmHandleV1 vm) noexcept {
    delete vm;
}

RsStatusV1 apiVmBindHostCallback(
    RsVmHandleV1 vm,
    const char* qualifiedName,
    RsHostCallbackV1 callback,
    void* userData) noexcept {
    if (!vm || !vm->bindings || !qualifiedName || !callback) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        ManagedHeap* vmHeap = vm->heap.get();
        const bool ok = vm->bindings->bind(
            std::string(qualifiedName),
            [callback, userData, vmHeap](
                const bytecode::FunctionReference&,
                ValueSpan args,
                RuntimeError& error) -> std::optional<Value> {
                std::vector<RsValueV1> abiArgs(args.size());
                for (std::size_t i = 0; i < args.size(); ++i) {
                    if (marshalValueToAbi(vmHeap, args[i], &abiArgs[i]) != RS_STATUS_V1_OK) {
                        error.code = ErrorCode::InvalidArguments;
                        error.message = "failed to marshal argument to C11 host callback";
                        return std::nullopt;
                    }
                }
                RsValueV1 abiOut{};
                const RsStatusV1 st = callback(
                    userData,
                    abiArgs.empty() ? nullptr : abiArgs.data(),
                    static_cast<uint32_t>(abiArgs.size()),
                    &abiOut);
                if (st != RS_STATUS_V1_OK) {
                    error.code = ErrorCode::ExecutionTerminated;
                    error.message = "C11 host callback returned error status";
                    return std::nullopt;
                }
                Value retVal{};
                if (!marshalValueFromAbi(abiOut, retVal)) {
                    error.code = ErrorCode::TypeMismatch;
                    error.message = "invalid return value from C11 host callback";
                    return std::nullopt;
                }
                return retVal;
            });
        return ok ? RS_STATUS_V1_OK : RS_STATUS_V1_INVALID_ARGUMENT;
    } catch (...) {
        return RS_STATUS_V1_OUT_OF_MEMORY;
    }
}

RsStatusV1 apiVmInvoke(
    RsVmHandleV1 vm,
    const char* qualifiedName,
    const RsValueV1* arguments,
    uint32_t argumentCount,
    RsValueV1* outResult) noexcept {
    if (!vm || !vm->interpreter || !qualifiedName ||
        (argumentCount > 0 && !arguments)) {
        return RS_STATUS_V1_INVALID_ARGUMENT;
    }
    try {
        std::vector<Value> cppArgs(argumentCount);
        for (uint32_t i = 0; i < argumentCount; ++i) {
            if (!marshalValueFromAbi(arguments[i], cppArgs[i])) {
                return RS_STATUS_V1_INVALID_ARGUMENT;
            }
        }
        const auto res = vm->interpreter->invoke(qualifiedName, cppArgs);
        if (!res.succeeded) {
            return mapRuntimeError(res.error.code);
        }
        if (outResult) {
            return marshalValueToAbi(vm->heap.get(), res.value, outResult);
        }
        return RS_STATUS_V1_OK;
    } catch (...) {
        return RS_STATUS_V1_RUNTIME_ERROR;
    }
}

const RsHostApiV1 kHostApiV1 = {
    sizeof(RsHostApiV1),
    RS_HOST_ABI_MAJOR_V1,
    RS_HOST_ABI_MINOR_V1,
    &apiHeapCreate,
    &apiHeapDestroy,
    &apiHeapId,
    &apiHeapCollect,
    &apiHeapCollectBudgetNs,
    &apiHeapPinObject,
    &apiHeapUnpinRoot,
    &apiHeapIsAlive,
    &apiHeapSetSweepCallback,
    &apiWeakFromObject,
    &apiWeakTryResolve,
    &apiHeapEnableDirtyTracking,
    &apiHeapMarkDirtyBit,
    &apiHeapPeekDirtyMask,
    &apiHeapConsumeDirtyMask,
    &apiHeapReadFieldWithPolicy,
    &apiHeapWriteFieldWithPolicy,
    &apiStringAllocUtf8,
    &apiStringAllocUtf16,
    &apiStringReadUtf8,
    &apiStringReadUtf16,
    &apiProgramLoadRsbc,
    &apiProgramDestroy,
    &apiVmCreate,
    &apiVmDestroy,
    &apiVmBindHostCallback,
    &apiVmInvoke,
};

} // namespace

extern "C" const RsHostApiV1* rs_get_host_api_v1(void) {
    return &kHostApiV1;
}
