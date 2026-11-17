/* Shaderc implements the same HLSL register layout as the PC Vulkan backend. */
#include "rhi.h"
#include "rhi_vulkan_bindings.h"

#include <shaderc/shaderc.h>
#include <spirv/unified1/spirv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int annotation(unsigned op)
{
    return op == SpvOpDecorate || op == SpvOpMemberDecorate ||
           op == SpvOpDecorationGroup || op == SpvOpDecorateId ||
           op == SpvOpDecorateString || op == SpvOpMemberDecorateString;
}

/* glslang puts the HLSL semantics in decoration strings. The renderer
 * already reads them from OpName, so convert them before creating the Vulkan
 * module. This also avoids requiring the Google HLSL extension on the GPU. */
static size_t normalize_semantics(const uint32_t *code, size_t words, uint32_t *out)
{
    if (words < 5 || code[0] != SpvMagicNumber || code[3] == 0)
        return 0;
    uint32_t bound = code[3];
    size_t *semantics = calloc(bound, sizeof *semantics);
    if (!semantics)
        return 0;
    const char view_semantic[] = "TSFPVIEWINDEX0";
    uint32_t view_index = 0;

    for (size_t i = 5; i < words;) {
        unsigned length = code[i] >> 16;
        unsigned op = code[i] & 0xffff;
        if (length == 0 || length > words - i) {
            free(semantics);
            return 0;
        }
        if (op == SpvOpDecorateString && length >= 4 && code[i + 1] < bound &&
            code[i + 2] == SpvDecorationUserSemantic) {
            if ((length - 3) * sizeof *code >= sizeof view_semantic &&
                !memcmp(code + i + 3, view_semantic, sizeof view_semantic))
                view_index = code[i + 1];
            else
                semantics[code[i + 1]] = i;
        }
        i += length;
    }

    memcpy(out, code, 5 * sizeof *out);
    size_t used = 5;
    if (view_index) {
        /* Vulkan 1.1 uses SPIR-V 1.3. Shaderc treats this private HLSL
         * semantic as an ordinary input until it is mapped here. */
        if (code[1] < 0x00010300u) {
            free(semantics);
            return 0;
        }
        out[used++] = (2u << 16) | SpvOpCapability;
        out[used++] = SpvCapabilityMultiView;
    }
    int named = 0;
    for (size_t i = 5; i < words;) {
        unsigned length = code[i] >> 16;
        unsigned op = code[i] & 0xffff;
        if (!named && annotation(op)) {
            for (uint32_t id = 0; id < bound; id++) {
                size_t at = semantics[id];
                if (!at)
                    continue;
                unsigned name_words = (code[at] >> 16) - 3;
                out[used++] = ((name_words + 2) << 16) | SpvOpName;
                out[used++] = id;
                memcpy(out + used, code + at + 3, name_words * sizeof *out);
                used += name_words;
            }
            named = 1;
        }

        int replaced_name = op == SpvOpName && length >= 3 &&
            code[i + 1] < bound && semantics[code[i + 1]];
        const char *extension = (const char *)(code + i + 1);
        int hlsl_extension = op == SpvOpExtension && length >= 2 &&
            (!strcmp(extension, "SPV_GOOGLE_hlsl_functionality1") ||
             !strcmp(extension, "SPV_GOOGLE_decorate_string"));
        if (view_index && op == SpvOpDecorate && length == 4 && code[i + 1] == view_index &&
            code[i + 2] == SpvDecorationLocation) {
            out[used++] = (4u << 16) | SpvOpDecorate;
            out[used++] = view_index;
            out[used++] = SpvDecorationBuiltIn;
            out[used++] = SpvBuiltInViewIndex;
        } else if (!replaced_name && !hlsl_extension &&
            op != SpvOpDecorateString && op != SpvOpMemberDecorateString) {
            memcpy(out + used, code + i, length * sizeof *out);
            used += length;
        }
        i += length;
    }
    free(semantics);
    return used;
}

int rhi_vk_shaderc_compile(uint32_t stage, const RhiShaderSource *source,
                           uint32_t **spirv, size_t *spirv_bytes,
                           char *error, size_t error_size)
{
    *spirv = NULL;
    *spirv_bytes = 0;
    if (error && error_size)
        error[0] = '\0';
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    shaderc_compilation_result_t result = NULL;
    if (!compiler || !options)
        goto done;

    shaderc_compile_options_set_source_language(options, shaderc_source_language_hlsl);
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    shaderc_compile_options_set_hlsl_io_mapping(options, true);
    shaderc_compile_options_set_hlsl_functionality1(options, true);
    shaderc_compile_options_set_auto_map_locations(options, true);
    shaderc_compile_options_set_binding_base(options, shaderc_uniform_kind_buffer,
        stage == RHI_STAGE_VERTEX ? RHI_VK_VS_UNIFORM_BASE : RHI_VK_PS_UNIFORM_BASE);
    shaderc_compile_options_set_binding_base(options, shaderc_uniform_kind_texture, RHI_VK_TEXTURE_BASE);
    shaderc_compile_options_set_binding_base(options, shaderc_uniform_kind_sampler, RHI_VK_SAMPLER_BASE);
    for (const RhiMacro *macro = source->macros; macro && macro->name; macro++) {
        shaderc_compile_options_add_macro_definition(options, macro->name, strlen(macro->name),
            macro->value, macro->value ? strlen(macro->value) : 0);
    }
    result = shaderc_compile_into_spv(compiler, source->hlsl, source->len,
        stage == RHI_STAGE_VERTEX ? shaderc_vertex_shader : shaderc_fragment_shader,
        source->name ? source->name : "game.hlsl", source->entry ? source->entry : "main", options);
    if (!result)
        goto done;
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
        if (error && error_size)
            snprintf(error, error_size, "%s", shaderc_result_get_error_message(result));
        goto done;
    }

    size_t size = shaderc_result_get_length(result);
    if (size % sizeof(uint32_t) != 0)
        goto done;
    uint32_t *buffer = malloc(size + 2 * sizeof(uint32_t));
    if (!buffer)
        goto done;
    size_t count = normalize_semantics((const uint32_t *)shaderc_result_get_bytes(result),
                                       size / sizeof *buffer, buffer);
    if (count == 0) {
        free(buffer);
        goto done;
    }
    *spirv = buffer;
    *spirv_bytes = count * sizeof *buffer;

done:
    if (result)
        shaderc_result_release(result);
    if (options)
        shaderc_compile_options_release(options);
    if (compiler)
        shaderc_compiler_release(compiler);
    if (!*spirv && error && error_size && !error[0])
        snprintf(error, error_size, "Shaderc did not produce a usable SPIR-V module");
    return *spirv != NULL;
}
