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

/*
 * Host ABI V2: Strictly additive extension embedding RsHostApiV1 at offset 0.
 * Zero V1 structs or fields are altered; legacy V1 consumers remain ABI-frozen.
 */

enum {
    RS_HOST_ABI_MAJOR_V2 = 2,
    RS_HOST_ABI_MINOR_V2 = 0,
};

enum {
    RS_VALUE_KIND_V2_NULL_ARRAY = 9,
    RS_VALUE_KIND_V2_FIXED16 = 10,
    RS_VALUE_KIND_V2_CHAR = 11,
    RS_VALUE_KIND_V2_SPAN = 12,
};

typedef uint8_t RsTypeKindV2;
enum {
    RS_TYPE_KIND_V2_CLASS = 0,
    RS_TYPE_KIND_V2_STRUCT = 1,
    RS_TYPE_KIND_V2_ENUM = 2,
    RS_TYPE_KIND_V2_INTERFACE = 3,
    RS_TYPE_KIND_V2_DELEGATE = 4,
};

typedef uint8_t RsPrimitiveKindV2;
enum {
    RS_PRIMITIVE_V2_VOID = 0,
    RS_PRIMITIVE_V2_BOOL = 1,
    RS_PRIMITIVE_V2_BYTE = 2,
    RS_PRIMITIVE_V2_SBYTE = 3,
    RS_PRIMITIVE_V2_SHORT = 4,
    RS_PRIMITIVE_V2_USHORT = 5,
    RS_PRIMITIVE_V2_INT = 6,
    RS_PRIMITIVE_V2_UINT = 7,
    RS_PRIMITIVE_V2_LONG = 8,
    RS_PRIMITIVE_V2_ULONG = 9,
    RS_PRIMITIVE_V2_FLOAT = 10,
    RS_PRIMITIVE_V2_DOUBLE = 11,
    RS_PRIMITIVE_V2_CHAR = 12,
    RS_PRIMITIVE_V2_STRING = 13,
    RS_PRIMITIVE_V2_OBJECT = 14,
    RS_PRIMITIVE_V2_STRUCT = 15,
    RS_PRIMITIVE_V2_ARRAY = 16,
    RS_PRIMITIVE_V2_ENUM = 17,
    RS_PRIMITIVE_V2_HANDLE = 18,
    RS_PRIMITIVE_V2_UNKNOWN = 255,
};

typedef uint32_t RsErrorCodeV2;
enum {
    RS_ERROR_CODE_V2_NONE = 0,
    RS_ERROR_CODE_V2_FUNCTION_NOT_FOUND = 1,
    RS_ERROR_CODE_V2_INVALID_ARGUMENTS = 2,
    RS_ERROR_CODE_V2_TYPE_MISMATCH = 3,
    RS_ERROR_CODE_V2_DIVISION_BY_ZERO = 4,
    RS_ERROR_CODE_V2_INTEGER_OVERFLOW = 5,
    RS_ERROR_CODE_V2_INSTRUCTION_BUDGET_EXCEEDED = 6,
    RS_ERROR_CODE_V2_RECURSION_LIMIT_EXCEEDED = 7,
    RS_ERROR_CODE_V2_EXTERNAL_FUNCTION_UNRESOLVED = 8,
    RS_ERROR_CODE_V2_DUPLICATE_SYMBOL = 9,
    RS_ERROR_CODE_V2_INVALID_OBJECT_REFERENCE = 10,
    RS_ERROR_CODE_V2_INVALID_NATIVE_HANDLE = 11,
    RS_ERROR_CODE_V2_NULL_REFERENCE = 12,
    RS_ERROR_CODE_V2_INDEX_OUT_OF_RANGE = 13,
    RS_ERROR_CODE_V2_OUT_OF_MEMORY = 14,
    RS_ERROR_CODE_V2_INVALID_PROGRAM = 15,
    RS_ERROR_CODE_V2_EXECUTION_TERMINATED = 16,
    RS_ERROR_CODE_V2_DETERMINISM_VIOLATION = 17,
    RS_ERROR_CODE_V2_REPLAY_MISMATCH = 18,
    RS_ERROR_CODE_V2_SCRIPT_EXCEPTION = 19,
    RS_ERROR_CODE_V2_CAPABILITY_DENIED = 20,
    RS_ERROR_CODE_V2_CROSS_HEAP_VIOLATION = 21,
};

typedef struct RsDomainOpaque RsDomainOpaque;
typedef RsDomainOpaque* RsDomainHandleV2;

typedef struct RsErrorInfoV2 {
    RsStatusV1 status;
    RsErrorCodeV2 error_code;
    uint32_t frame_count;
    uint32_t reserved;
    const char* utf8_message;
    const char* exception_type_utf8;
} RsErrorInfoV2;

typedef struct RsErrorFrameV2 {
    const char* function_utf8;
    const char* module_utf8;
    const char* source_file_utf8;
    uint32_t line;
    uint32_t column;
} RsErrorFrameV2;

typedef void (*RsAsyncResumeFnV2)(void* resume_ctx);

typedef struct RsAwaitSchedulerCallbacksV2 {
    uint32_t struct_size;
    uint32_t reserved;
    void* user_data;
    uint8_t (*handles_value)(void* user_data, const RsValueV1* awaited);
    uint8_t (*is_completed)(void* user_data, const RsValueV1* awaited);
    RsStatusV1 (*try_get_result)(
        void* user_data,
        const RsValueV1* awaited,
        uint8_t has_result,
        RsValueV1* out_result,
        char* err_utf8_buf,
        size_t err_utf8_cap);
    RsStatusV1 (*on_suspend)(
        void* user_data,
        const RsValueV1* awaited,
        RsAsyncResumeFnV2 resume_fn,
        void* resume_ctx,
        char* err_utf8_buf,
        size_t err_utf8_cap);
    RsStatusV1 (*try_resolve_async_return)(
        void* user_data,
        uint64_t return_type_id,
        const RsValueV1* optional_payload,
        RsValueV1* out_wrapped_return,
        uint8_t* out_handled,
        char* err_utf8_buf,
        size_t err_utf8_cap);
} RsAwaitSchedulerCallbacksV2;

typedef struct RsTypeInfoV2 {
    uint64_t type_id;
    uint64_t base_type_id;
    const char* name_utf8;
    const char* module_utf8;
    RsTypeKindV2 kind;
    uint8_t is_blittable;
    uint16_t reserved;
    uint32_t field_count;
    uint32_t native_byte_size;
    uint32_t reserved2;
} RsTypeInfoV2;

typedef struct RsFieldInfoV2 {
    const char* name_utf8;
    const char* former_names_utf8;
    uint64_t type_id;
    RsPrimitiveKindV2 primitive_kind;
    uint8_t has_field_tag;
    uint8_t is_synthetic;
    uint8_t reserved;
    int32_t field_tag;
    uint32_t alias_count;
    uint32_t byte_offset;
    uint32_t field_index;
    uint32_t reserved2;
} RsFieldInfoV2;

typedef struct RsHostApiV2 {
    RsHostApiV1 v1;
    uint32_t v2_struct_size;
    uint32_t v2_abi_major;
    uint32_t v2_abi_minor;
    uint32_t v2_reserved;

    /* Domain & Heap Reclamation */
    RsStatusV1 (*domain_create)(RsDomainHandleV2* out_domain);
    void (*domain_destroy)(RsDomainHandleV2 domain);
    RsStatusV1 (*domain_collect)(
        RsDomainHandleV2 domain,
        uint64_t* out_reclaimed_bytes);
    RsStatusV1 (*heap_create_in_domain)(
        RsDomainHandleV2 domain,
        size_t initial_threshold_bytes,
        size_t max_heap_bytes,
        RsHeapHandleV1* out_heap);
    RsStatusV1 (*heap_trim_excess)(
        RsHeapHandleV1 heap,
        uint64_t* out_trimmed_bytes);

    /* Last Error & Script Stack Trace */
    RsStatusV1 (*vm_get_last_error)(
        RsVmHandleV1 vm,
        RsErrorInfoV2* out_error_info);
    RsStatusV1 (*vm_get_last_error_frame)(
        RsVmHandleV1 vm,
        uint32_t frame_index,
        RsErrorFrameV2* out_frame);
    void (*vm_clear_last_error)(RsVmHandleV1 vm);

    /* Budget & C Async Scheduler (0 instruction_budget resets default VM limit) */
    RsStatusV1 (*vm_set_instruction_budget)(
        RsVmHandleV1 vm,
        uint64_t instruction_budget);
    RsStatusV1 (*vm_get_last_executed_instructions)(
        RsVmHandleV1 vm,
        uint64_t* out_count);
    RsStatusV1 (*vm_set_await_scheduler)(
        RsVmHandleV1 vm,
        const RsAwaitSchedulerCallbacksV2* callbacks);

    /* Program Introspection & JSON ABI Export */
    RsStatusV1 (*program_type_count)(
        RsProgramHandleV1 program,
        uint32_t* out_count);
    RsStatusV1 (*program_get_type_info)(
        RsProgramHandleV1 program,
        uint32_t type_index,
        RsTypeInfoV2* out_info);
    RsStatusV1 (*program_find_type)(
        RsProgramHandleV1 program,
        const char* qualified_name,
        uint64_t* out_type_id);
    RsStatusV1 (*program_get_field_info)(
        RsProgramHandleV1 program,
        uint64_t type_id,
        uint32_t field_index,
        RsFieldInfoV2* out_info);
    RsStatusV1 (*program_find_field_index)(
        RsProgramHandleV1 program,
        uint64_t type_id,
        const char* field_name,
        uint32_t* out_field_index);
    RsStatusV1 (*program_find_function_id)(
        RsProgramHandleV1 program,
        const char* qualified_function_name,
        uint64_t* out_function_id);
    RsStatusV1 (*program_export_abi_json)(
        RsProgramHandleV1 program,
        char* out_json_buf,
        size_t buf_capacity_bytes,
        size_t* out_required_bytes);

    /* Invocation, Heap Object/Array/Buffer & Blittable Struct Marshaling */
    RsStatusV1 (*vm_invoke_id_v2)(
        RsVmHandleV1 vm,
        uint64_t function_id,
        const RsValueV1* arguments,
        uint32_t argument_count,
        RsValueV1* out_result);
    RsStatusV1 (*vm_invoke_method_v2)(
        RsVmHandleV1 vm,
        RsObjectRefV1 receiver,
        const char* method_name_utf8,
        const RsValueV1* arguments,
        uint32_t argument_count,
        RsValueV1* out_result);
    RsStatusV1 (*heap_alloc_record)(
        RsHeapHandleV1 heap,
        RsProgramHandleV1 program,
        uint64_t type_id,
        uint32_t untyped_field_count,
        RsObjectRefV1* out_ref);
    RsStatusV1 (*heap_alloc_array)(
        RsHeapHandleV1 heap,
        RsPrimitiveKindV2 element_primitive,
        uint64_t element_type_id,
        uint32_t length,
        RsObjectRefV1* out_array_ref);
    RsStatusV1 (*heap_array_length)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 array_ref,
        uint32_t* out_length);
    RsStatusV1 (*heap_array_get_v2)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 array_ref,
        uint32_t index,
        RsValueV1* out_element);
    RsStatusV1 (*heap_array_set_v2)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 array_ref,
        uint32_t index,
        const RsValueV1* value);
    RsStatusV1 (*heap_pack_blittable_struct)(
        RsHeapHandleV1 heap,
        RsProgramHandleV1 program,
        uint64_t struct_type_id,
        const RsValueV1* field_values,
        uint32_t field_count,
        void* out_buf,
        size_t buf_capacity_bytes,
        size_t* out_required_bytes);
    RsStatusV1 (*heap_unpack_blittable_struct)(
        RsHeapHandleV1 heap,
        RsProgramHandleV1 program,
        uint64_t struct_type_id,
        const void* in_buf,
        size_t buf_size_bytes,
        RsValueV1* out_field_values,
        uint32_t max_fields,
        uint32_t* out_field_count);
    RsStatusV1 (*heap_wrap_native_buffer)(
        RsHeapHandleV1 heap,
        void* data_ptr,
        size_t byte_length,
        uint8_t read_only,
        RsObjectRefV1* out_buffer_ref);
    RsStatusV1 (*heap_revoke_native_buffer)(
        RsHeapHandleV1 heap,
        RsObjectRefV1 buffer_ref);
} RsHostApiV2;

const RsHostApiV2* rs_get_host_api_v2(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REALSCRIPT_HOST_ABI_H */
