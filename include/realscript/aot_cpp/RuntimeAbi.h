#pragma once

#include <stdint.h>

// Stable, source-compatible C discovery boundary for generated RealScript AOT
// modules. Executable entry points use opaque pointers so the public ABI remains
// independent from the C++ runtime implementation behind an explicit version
// check.
#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t RsStatusV1;
enum {
    RS_STATUS_V1_OK = 0,
    RS_STATUS_V1_INVALID_ARGUMENT = 1,
    RS_STATUS_V1_ABI_MISMATCH = 2,
    RS_STATUS_V1_RUNTIME_ERROR = 3,
};

typedef uint32_t RsBackendKindV1;
enum {
    RS_BACKEND_V1_INVALID = 0,
    RS_BACKEND_V1_NATIVE_AOT = 2,
};

typedef struct RsRuntimeApiV1 {
    uint32_t size;
    uint32_t abi_major;
    uint32_t abi_minor;
    void* user_data;
} RsRuntimeApiV1;

typedef RsStatusV1 (*RsAotEntryPointV1)(
    void* execution_context,
    const void* arguments,
    uint32_t argument_count,
    void* result);

typedef struct RsFunctionEntryV1 {
    uint32_t size;
    uint32_t backend_kind;
    uint64_t function_id;
    uint64_t version;
    RsAotEntryPointV1 entry_point;
    const void* backend_data;
    const void* debug_info;
} RsFunctionEntryV1;

typedef struct RsModuleExportsV1 {
    uint32_t size;
    uint32_t required_abi_major;
    uint32_t required_abi_minor;
    const char* module_name;
    uint64_t content_hash;
    const RsFunctionEntryV1* functions;
    uint32_t function_count;
    const void* program_descriptor;
} RsModuleExportsV1;

typedef RsStatusV1 (*RsModuleQueryV1)(
    const RsRuntimeApiV1* runtime_api,
    RsModuleExportsV1* out_exports);

#ifdef __cplusplus
} // extern "C"
#endif
