#ifndef REALSCRIPT_COMPILER_PLUGIN_ABI_H
#define REALSCRIPT_COMPILER_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RS_COMPILER_PLUGIN_ABI_VERSION_1 1u
#define RS_COMPILER_MACRO_PLUGIN_ENTRY_V1 "rs_get_compiler_macro_plugin_v1"

#if defined(_WIN32)
#define RS_COMPILER_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RS_COMPILER_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define RS_COMPILER_PLUGIN_EXPORT
#endif

typedef enum RsCompilerMacroKindV1 {
    RS_COMPILER_MACRO_ATTRIBUTE_V1 = 0,
    RS_COMPILER_MACRO_EXPRESSION_V1 = 1
} RsCompilerMacroKindV1;

typedef enum RsCompilerDiagSeverityV1 {
    RS_COMPILER_DIAG_ERROR_V1 = 0,
    RS_COMPILER_DIAG_WARNING_V1 = 1
} RsCompilerDiagSeverityV1;

typedef struct RsCompilerMacroHostV1 {
    void (*set_generated_source)(
        void* host_ctx,
        const char* utf8_source,
        size_t byte_len);
    void (*report_diagnostic)(
        void* host_ctx,
        const char* code_utf8,
        const char* message_utf8,
        uint8_t severity);
} RsCompilerMacroHostV1;

typedef struct RsCompilerMacroExpandInputV1 {
    const char* macro_name;
    uint32_t kind;
    const char* module_name;
    const char* source_path;
    const char* target_name;
    const char* target_declaration_utf8;
    const char* arguments_utf8;
    uint32_t arg_count;
    const char* const* arg_names;
    const char* const* arg_values;
    uint32_t line;
    uint32_t column;
} RsCompilerMacroExpandInputV1;

typedef struct RsCompilerMacroDescriptorV1 {
    const char* macro_name;
    uint32_t kind;
} RsCompilerMacroDescriptorV1;

typedef struct RsCompilerMacroPluginV1 {
    size_t struct_size;
    uint32_t abi_version;
    const char* plugin_name;
    const char* plugin_version;
    size_t macro_count;
    const RsCompilerMacroDescriptorV1* macros;
    void (*expand)(
        void* plugin_user_data,
        const RsCompilerMacroExpandInputV1* input,
        const RsCompilerMacroHostV1* host,
        void* host_ctx);
    void (*cleanup)(void* plugin_user_data);
    void* plugin_user_data;
} RsCompilerMacroPluginV1;

typedef const RsCompilerMacroPluginV1* (*RsGetCompilerMacroPluginV1Fn)(void);

#ifdef __cplusplus
}
#endif

#endif
