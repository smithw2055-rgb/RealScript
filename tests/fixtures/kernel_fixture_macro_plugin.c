#include "realscript/CompilerPluginAbi.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void rs_format_pascal_name(const char* raw, char* out, size_t cap) {
    size_t src = 0u;
    size_t dst = 0u;
    if (!out || cap == 0u) {
        return;
    }
    if (!raw) {
        out[0] = '\0';
        return;
    }
    while (raw[src] == '_') {
        ++src;
    }
    while (raw[src] != '\0' && dst + 1u < cap) {
        char ch = raw[src++];
        if (dst == 0u && ch >= 'a' && ch <= 'z') {
            ch = (char)('A' + (ch - 'a'));
        }
        out[dst++] = ch;
    }
    out[dst] = '\0';
}

static void rs_fixture_macro_expand(
    void* plugin_user_data,
    const RsCompilerMacroExpandInputV1* input,
    const RsCompilerMacroHostV1* host,
    void* host_ctx) {
    char buffer[4096];
    int written = -1;
    (void)plugin_user_data;

    if (!input || !host || !host->set_generated_source || !input->macro_name ||
        !input->module_name || input->module_name[0] == '\0') {
        return;
    }

    if (input->kind == (uint32_t)RS_COMPILER_MACRO_ATTRIBUTE_V1 &&
        strcmp(input->macro_name, "AutoGetter") == 0) {
        const char* type_kw = "class";
        if (!input->target_name || input->target_name[0] == '\0') {
            if (host->report_diagnostic) {
                host->report_diagnostic(
                    host_ctx,
                    "RS9900",
                    "AutoGetter attribute requires a target declaration",
                    (uint8_t)RS_COMPILER_DIAG_ERROR_V1);
            }
            return;
        }
        if (input->target_declaration_utf8 &&
            strstr(input->target_declaration_utf8, "struct ") != NULL &&
            strstr(input->target_declaration_utf8, "class ") == NULL) {
            type_kw = "struct";
        }
        if (input->arg_count >= 2u && input->arg_values &&
            input->arg_values[0] && input->arg_values[0][0] != '\0' &&
            input->arg_values[1] && input->arg_values[1][0] != '\0') {
            written = snprintf(
                buffer,
                sizeof(buffer),
                "module %s;\n"
                "public partial %s %s {\n"
                "    public int Get%s() { return this.%s; }\n"
                "}\n",
                input->module_name,
                type_kw,
                input->target_name,
                input->arg_values[0],
                input->arg_values[1]);
        } else if (input->arg_count >= 1u && input->arg_values &&
                   input->arg_values[0] && input->arg_values[0][0] != '\0') {
            char prop_name[128];
            rs_format_pascal_name(input->arg_values[0], prop_name, sizeof(prop_name));
            if (prop_name[0] == '\0') {
                snprintf(prop_name, sizeof(prop_name), "AutoField");
            }
            written = snprintf(
                buffer,
                sizeof(buffer),
                "module %s;\n"
                "public partial %s %s {\n"
                "    public int Get%s() { return this.%s; }\n"
                "    public int GetAutoValue() { return 108; }\n"
                "}\n",
                input->module_name,
                type_kw,
                input->target_name,
                prop_name,
                input->arg_values[0]);
        } else {
            written = snprintf(
                buffer,
                sizeof(buffer),
                "module %s;\n"
                "public partial %s %s {\n"
                "    public int GetAutoValue() { return 108; }\n"
                "}\n",
                input->module_name,
                type_kw,
                input->target_name);
        }
    } else if (input->kind == (uint32_t)RS_COMPILER_MACRO_EXPRESSION_V1 &&
               strcmp(input->macro_name, "plugin_magic") == 0) {
        if (input->arg_count == 1u && input->arg_values && input->arg_values[0] &&
            strcmp(input->arg_values[0], "invalid") == 0) {
            if (host->report_diagnostic) {
                host->report_diagnostic(
                    host_ctx,
                    "RS9901",
                    "plugin_magic rejected invalid input",
                    (uint8_t)RS_COMPILER_DIAG_ERROR_V1);
            }
            return;
        }
        if (input->arg_count == 0u) {
            written = snprintf(
                buffer,
                sizeof(buffer),
                "module %s;\n"
                "public int plugin_magic() {\n"
                "    return 1337;\n"
                "}\n",
                input->module_name);
        } else {
            written = snprintf(
                buffer,
                sizeof(buffer),
                "module %s;\n"
                "public int plugin_magic(int v) {\n"
                "    return v + 1337;\n"
                "}\n",
                input->module_name);
        }
    }

    if (written > 0 && (size_t)written < sizeof(buffer)) {
        host->set_generated_source(host_ctx, buffer, (size_t)written);
    }
}

static const RsCompilerMacroDescriptorV1 kFixtureMacros[2] = {
    {"AutoGetter", (uint32_t)RS_COMPILER_MACRO_ATTRIBUTE_V1},
    {"plugin_magic", (uint32_t)RS_COMPILER_MACRO_EXPRESSION_V1}
};

static const RsCompilerMacroPluginV1 kFixtureMacroPlugin = {
    sizeof(RsCompilerMacroPluginV1),
    RS_COMPILER_PLUGIN_ABI_VERSION_1,
    "kernel_fixture_macro_plugin",
    "1.0.0",
    2u,
    kFixtureMacros,
    &rs_fixture_macro_expand,
    NULL,
    NULL
};

RS_COMPILER_PLUGIN_EXPORT const RsCompilerMacroPluginV1*
rs_get_compiler_macro_plugin_v1(void) {
    return &kFixtureMacroPlugin;
}
