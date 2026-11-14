#include "realscript/HostAbi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int check(int cond) {
    return cond ? 1 : 0;
}

int main(void) {
    const RsHostApiV1* api = rs_get_host_api_v1();
    if (!check(api != NULL) ||
        !check(api->struct_size == (uint32_t)sizeof(RsHostApiV1)) ||
        !check(api->abi_major == RS_HOST_ABI_MAJOR_V1) ||
        !check(api->abi_minor == RS_HOST_ABI_MINOR_V1)) {
        return 1;
    }

    RsHeapHandleV1 heap = NULL;
    if (!check(api->heap_create(4096u * 1024u, &heap) == RS_STATUS_V1_OK) ||
        !check(heap != NULL) ||
        !check(api->heap_id(heap) != 0u)) {
        return 2;
    }

    static const char kUtf8Sample[] = "realscript-c11-host";
    RsObjectRefV1 str_ref = {0};
    if (!check(api->string_alloc_utf8(
                   heap, kUtf8Sample, sizeof(kUtf8Sample) - 1u, &str_ref) == RS_STATUS_V1_OK)) {
        api->heap_destroy(heap);
        return 3;
    }

    const char* read_ptr = NULL;
    size_t read_len = 0;
    if (!check(api->string_read_utf8(heap, str_ref, &read_ptr, &read_len) == RS_STATUS_V1_OK) ||
        !check(read_len == sizeof(kUtf8Sample) - 1u) ||
        !check(memcmp(read_ptr, kUtf8Sample, read_len) == 0)) {
        api->heap_destroy(heap);
        return 4;
    }

    uint16_t utf16_buf[64] = {0};
    size_t required_units = 0;
    if (!check(api->string_read_utf16(
                   heap, str_ref, utf16_buf, 64u, &required_units) == RS_STATUS_V1_OK) ||
        !check(required_units == sizeof(kUtf8Sample) - 1u) ||
        !check(utf16_buf[0] == (uint16_t)'r')) {
        api->heap_destroy(heap);
        return 5;
    }

    uint64_t root_token = 0;
    if (!check(api->heap_pin_object(heap, str_ref, &root_token) == RS_STATUS_V1_OK) ||
        !check(api->heap_collect(heap) == RS_STATUS_V1_OK) ||
        !check(api->heap_is_alive(heap, str_ref) != 0u)) {
        api->heap_destroy(heap);
        return 6;
    }

    const RsWeakRefV1 weak = api->weak_from_object(str_ref);
    RsObjectRefV1 resolved = {0};
    if (!check(api->weak_try_resolve(heap, weak, &resolved) == RS_STATUS_V1_OK) ||
        !check(resolved.slot == str_ref.slot && resolved.generation == str_ref.generation)) {
        api->heap_destroy(heap);
        return 7;
    }

    if (!check(api->heap_unpin_root(heap, root_token) == RS_STATUS_V1_OK) ||
        !check(api->heap_collect(heap) == RS_STATUS_V1_OK) ||
        !check(api->heap_is_alive(heap, str_ref) == 0u)) {
        api->heap_destroy(heap);
        return 8;
    }

    api->heap_destroy(heap);
    return 0;
}
