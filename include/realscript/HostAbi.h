#ifndef REALSCRIPT_HOST_ABI_H
#define REALSCRIPT_HOST_ABI_H


#include "realscript/aot_cpp/RuntimeAbi.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Versioned Pure C11 Host & Interop ABI for RealScript L0 Microkernel.
 *
 * Designed for zero-exception cross-runtime bridges (.NET CLR / WPF P/Invoke,
 * Rust, C/C++ game hosts, and Emscripten WebAssembly). No C++ STL types or
 * C++ exceptions ever cross this boundary.
 */
#ifdef __cplusplus
extern "C" {
#endif

enum {
    RS_HOST_ABI_MAJOR_V1 = 1,
    RS_HOST_ABI_MINOR_V1 = 0,
};

enum {
    RS_STATUS_V1_OUT_OF_MEMORY = 4,
    RS_STATUS_V1_CROSS_HEAP_VIOLATION = 5,
    RS_STATUS_V1_STALE_REFERENCE = 6,
    RS_STATUS_V1_BUFFER_TOO_SMALL = 7,
};

typedef uint8_t RsValueKindV1;
enum {
    RS_VALUE_KIND_V1_VOID = 0,
    RS_VALUE_KIND_V1_BOOL = 1,
    RS_VALUE_KIND_V1_INT64 = 2,
    RS_VALUE_KIND_V1_UINT64 = 3,
    RS_VALUE_KIND_V1_DOUBLE = 4,
    RS_VALUE_KIND_V1_OBJECT_REF = 5,
    RS_VALUE_KIND_V1_NATIVE_HANDLE = 6,
    RS_VALUE_KIND_V1_NULL_OBJECT = 7,
    RS_VALUE_KIND_V1_NULL_STRING = 8,
};

typedef uint8_t RsCrossHeapPolicyV1;
enum {
    RS_CROSS_HEAP_V1_ALLOW_READ_WRITE = 0,
    RS_CROSS_HEAP_V1_READ_ONLY_COPY_STRING = 1,
    RS_CROSS_HEAP_V1_DENY = 2,
};

typedef struct RsHeapOpaque RsHeapOpaque;
typedef struct RsProgramOpaque RsProgramOpaque;
typedef struct RsVmOpaque RsVmOpaque;

typedef RsHeapOpaque* RsHeapHandleV1;
typedef RsProgramOpaque* RsProgramHandleV1;
typedef RsVmOpaque* RsVmHandleV1;

typedef struct RsObjectRefV1 {
    uint32_t slot;
    uint32_t generation;
    uint32_t heap_id;
    uint8_t object_kind;
    uint8_t reserved[3];
} RsObjectRefV1;

typedef struct RsNativeHandleV1 {
    uint32_t slot;
    uint32_t generation;
    uint64_t type_id;
    uint32_t registry_id;
    uint32_t reserved;
} RsNativeHandleV1;

typedef struct RsWeakRefV1 {
    uint32_t slot;
    uint32_t generation;
    uint32_t heap_id;
    uint8_t object_kind;
    uint8_t reserved[3];
} RsWeakRefV1;

typedef struct RsValueV1 {
    RsValueKindV1 kind;
    uint8_t reserved[7];
    union {
        uint8_t bool_val;
        int64_t i64_val;
        uint64_t u64_val;
        double f64_val;
        RsObjectRefV1 obj_ref;
        RsNativeHandleV1 native_handle;
    } payload;
} RsValueV1;

typedef void (*RsNativeSweepCallbackV1)(
    void* user_data,
    RsNativeHandleV1 handle,
    uint64_t heap_id,
    RsObjectRefV1 swept_wrapper);

typedef RsStatusV1 (*RsHostCallbackV1)(
    void* user_data,
    const RsValueV1* arguments,
    uint32_t argument_count,
    RsValueV1* out_result);

typedef struct RsHostApiV1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;

    /* ManagedHeap lifecycle, pinning, and nanosecond-budget incremental GC */
    RsStatusV1 (*heap_create)(size_t max_bytes, RsHeapHandleV1* out_heap);
    void (*heap_destroy)(RsHeapHandleV1 heap);
    uint64_t (*heap_id)(RsHeapHandleV1 heap);
    RsStatusV1 (*heap_collect)(RsHeapHandleV1 heap);
    RsStatusV1 (*heap_collect_budget_ns)(
        RsHeapHandleV1 heap,
        uint64_t budget_nanoseconds,
        uint8_t* out_cycle_completed);
    RsStatusV1 (*heap_pin_object)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 object_ref,
        uint64_t* out_root_token);
    RsStatusV1 (*heap_unpin_root)(
        RsHeapHandleV1 heap,
        uint64_t root_token);
    uint8_t (*heap_is_alive)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 object_ref);
    RsStatusV1 (*heap_set_sweep_callback)(
        RsHeapHandleV1 heap,
        RsNativeSweepCallbackV1 callback,
        void* user_data);

    /* WeakRef helpers */
    RsWeakRefV1 (*weak_from_object)(RsObjectRefV1 object_ref);
    RsStatusV1 (*weak_try_resolve)(
        RsHeapHandleV1 heap,
        RsWeakRefV1 weak_ref,
        RsObjectRefV1* out_object_ref);

    /* Zero-default-overhead 64-bit dirty bitmask & cross-heap field access (MVVM / Dual-Heap HUD) */
    RsStatusV1 (*heap_enable_dirty_tracking)(
        RsHeapHandleV1 heap,
        uint64_t type_id,
        const uint8_t* field_to_bit_map,
        uint32_t field_count);
    RsStatusV1 (*heap_mark_dirty_bit)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 object_ref,
        uint8_t bit_index);
    uint64_t (*heap_peek_dirty_mask)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 object_ref);
    uint64_t (*heap_consume_dirty_mask)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 object_ref);
    RsStatusV1 (*heap_read_field_with_policy)(
        RsHeapHandleV1 caller_heap,
        RsHeapHandleV1 target_heap,
        RsObjectRefV1 target_object,
        uint32_t field_index,
        RsCrossHeapPolicyV1 policy,
        RsValueV1* out_value);
    RsStatusV1 (*heap_write_field_with_policy)(
        RsHeapHandleV1 caller_heap,
        RsHeapHandleV1 target_heap,
        RsObjectRefV1 target_object,
        uint32_t field_index,
        RsCrossHeapPolicyV1 policy,
        const RsValueV1* new_value);

    /* UTF-8 & UTF-16 String Bridge (for .NET WPF System.String / ReadOnlySpan<char> & C++) */
    RsStatusV1 (*string_alloc_utf8)(
        RsHeapHandleV1 heap,
        const char* utf8_data,
        size_t utf8_byte_len,
        RsObjectRefV1* out_string_ref);
    RsStatusV1 (*string_alloc_utf16)(
        RsHeapHandleV1 heap,
        const uint16_t* utf16_data,
        size_t utf16_unit_len,
        RsObjectRefV1* out_string_ref);
    RsStatusV1 (*string_read_utf8)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 string_ref,
        const char** out_utf8_ptr,
        size_t* out_utf8_byte_len);
    RsStatusV1 (*string_read_utf16)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 string_ref,
        uint16_t* out_utf16_buf,
        size_t buf_capacity_units,
        size_t* out_required_units);

    /* Bytecode Program & VM Execution */
    RsStatusV1 (*program_load_rsbc)(
        const uint8_t* rsbc_bytes,
        size_t rsbc_size,
        RsProgramHandleV1* out_program);
    void (*program_destroy)(RsProgramHandleV1 program);
    RsStatusV1 (*vm_create)(
        RsProgramHandleV1 program,
        RsHeapHandleV1 heap,
        RsVmHandleV1* out_vm);
    void (*vm_destroy)(RsVmHandleV1 vm);
    RsStatusV1 (*vm_bind_host_callback)(
        RsVmHandleV1 vm,
        const char* qualified_function_name,
        RsHostCallbackV1 callback,
        void* user_data);
    RsStatusV1 (*vm_invoke)(
        RsVmHandleV1 vm,
        const char* qualified_function_name,
        const RsValueV1* arguments,
        uint32_t argument_count,
        RsValueV1* out_result);
} RsHostApiV1;

const RsHostApiV1* rs_get_host_api_v1(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REALSCRIPT_HOST_ABI_H */
