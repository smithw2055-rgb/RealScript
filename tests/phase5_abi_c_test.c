#include "realscript/aot_cpp/RuntimeAbi.h"

#include <stddef.h>
#include <stdint.h>

static RsStatusV1 sample_entry(
    void* execution_context,
    const void* arguments,
    uint32_t argument_count,
    void* result) {
    (void)execution_context;
    (void)arguments;
    (void)argument_count;
    (void)result;
    return RS_STATUS_V1_OK;
}

int main(void) {
    RsRuntimeApiV1 api = {
        (uint32_t)sizeof(RsRuntimeApiV1), 1u, 0u, NULL};
    RsFunctionEntryV1 entry = {
        (uint32_t)sizeof(RsFunctionEntryV1),
        RS_BACKEND_V1_NATIVE_AOT,
        UINT64_C(42),
        UINT64_C(1),
        &sample_entry,
        NULL,
        NULL};
    RsModuleExportsV1 exports = {
        (uint32_t)sizeof(RsModuleExportsV1),
        1u,
        0u,
        "C.Abi.Test",
        UINT64_C(7),
        &entry,
        1u,
        NULL};
    return api.size == (uint32_t)sizeof(api) &&
            exports.functions[0].entry_point == &sample_entry &&
            exports.functions[0].backend_kind == RS_BACKEND_V1_NATIVE_AOT
        ? 0
        : 1;
}
