#include "sniper_scope.h"
#include "stereo_backend.h"
#include "xr.h"

#include <stdio.h>
#include <string.h>

enum { SCOPE_UBO = 7, SCOPE_TEXTURE = 8 };

typedef struct {
    float eyes[32];
    XrVector4f center, right, up;
} ScopeConstants;

static struct {
    RhiShader *vertex, *pixel;
    RhiBuffer *constants;
    RhiSampler *sampler;
    RhiDepthState *depth;
    RhiRasterState *raster;
    RhiBlendState *blend;
    int attempted;
} scope;

static const char shader[] =
    "cbuffer Lens : register(b7) { float4 eyes[8]; float4 center; float4 right; float4 up; };\n"
    "struct Input { uint id : SV_VertexID; uint eye : TSFPVIEWINDEX0; };\n"
    "struct Output { float4 pos : SV_Position; float2 q : TEXCOORD0; };\n"
    "Output vs_main(Input i) {\n"
    "  Output o; o.q = float2(i.id & 1u, i.id >> 1u)*2-1;\n"
    "  float4 p = center + right*o.q.x - up*o.q.y;\n"
    "  uint r = (i.eye & 1u)*4u;\n"
    "  o.pos = float4(dot(eyes[r],p),dot(eyes[r+1],p),dot(eyes[r+2],p),dot(eyes[r+3],p));\n"
    "  return o;\n"
    "}\n"
    "Texture2D image : register(t8); SamplerState optics : register(s8);\n"
    "float4 ps_main(Output i) : SV_Target {\n"
    "  float r = length(i.q); clip(1-r);\n"
    "  float3 color = image.Sample(optics, i.q*0.54+0.5).rgb;\n"
    "  float2 q = abs(i.q);\n"
    "  float crosshair = min(max(q.x-0.22,q.y-0.012), max(q.y-0.22,q.x-0.012));\n"
    "  float aa = clamp(max(fwidth(i.q.x), fwidth(i.q.y))*0.5, 0.002, 0.035);\n"
    "  color *= lerp(0.32, 1, smoothstep(-aa,aa,crosshair-0.017));\n"
    "  color = lerp(color, float3(.94,.93,.81), 1-smoothstep(-aa,aa,crosshair));\n"
    "  float ring = smoothstep(.90,.95,r);\n"
    "  return float4(lerp(color, float3(.045,.049,.052), ring), 1);\n"
    "}\n";

void tsfp_scope_render_destroy(void)
{
    if (scope.vertex) rhi_shader_destroy(scope.vertex);
    if (scope.pixel) rhi_shader_destroy(scope.pixel);
    if (scope.constants) rhi_buffer_destroy(scope.constants);
    if (scope.sampler) rhi_sampler_destroy(scope.sampler);
    if (scope.depth) rhi_depth_state_destroy(scope.depth);
    if (scope.raster) rhi_raster_state_destroy(scope.raster);
    if (scope.blend) rhi_blend_state_destroy(scope.blend);
    memset(&scope, 0, sizeof scope);
}

static int initialize(void)
{
    if (scope.attempted)
        return scope.constants != NULL;
    scope.attempted = 1;
    char error[1024] = {0};
    RhiShaderSource source = {.hlsl = shader, .len = sizeof shader - 1, .name = "XR Sniper Lens",
        .entry = "vs_main", .target = "vs_4_0", .optimize = 1};
    scope.vertex = rhi_shader_create(RHI_STAGE_VERTEX, &source, error, sizeof error);
    if (!scope.vertex)
        goto fail;
    source.entry = "ps_main";
    source.target = "ps_4_0";
    scope.pixel = rhi_shader_create(RHI_STAGE_PIXEL, &source, error, sizeof error);
    if (!scope.pixel)
        goto fail;
    const RhiDepthDesc depth = {.depth_enable = 1, .depth_write = 1, .depth_func = RHI_CMP_LESS_EQUAL};
    const RhiRasterDesc raster = {.fill = RHI_FILL_SOLID, .cull = RHI_CULL_NONE, .depth_clip = 1};
    const RhiBlendDesc blend = {.write_mask = RHI_WRITE_ALL};
    const RhiSamplerDesc sampler = {.filter = RHI_FILTER_LINEAR, .address_u = RHI_ADDRESS_CLAMP,
        .address_v = RHI_ADDRESS_CLAMP, .address_w = RHI_ADDRESS_CLAMP};
    const RhiBufferDesc constants = {.size = sizeof(ScopeConstants), .usage = RHI_USAGE_DYNAMIC,
        .bind = RHI_BIND_UNIFORM, .cpu_access = RHI_CPU_WRITE};
    scope.sampler = rhi_sampler_create(&sampler);
    scope.depth = rhi_depth_state_create(&depth);
    scope.raster = rhi_raster_state_create(&raster);
    scope.blend = rhi_blend_state_create(&blend);
    if (!scope.sampler || !scope.depth || !scope.raster || !scope.blend)
        goto fail;
    scope.constants = rhi_buffer_create(&constants, NULL);
    if (scope.constants)
        return 1;

fail:
    fprintf(stderr, "[XR] Could not initialize the sniper lens renderer %s\n", error);
    tsfp_scope_render_destroy();
    scope.attempted = 1;
    return 0;
}

void tsfp_scope_render(RhiView *image)
{
    TsfpScopeGeometry geometry;
    ScopeConstants constants;
    RhiImage *world = tsfp_vk_uplink_source();
    if (!world || !tsfp_xr_scope_lens(&geometry, constants.eyes) || !initialize())
        return;
    const float r = geometry.radius;
    constants.center = (XrVector4f){geometry.center.x, geometry.center.y, geometry.center.z, 1};
    constants.right = (XrVector4f){r*geometry.right.x, r*geometry.right.y, r*geometry.right.z, 0};
    constants.up = (XrVector4f){r*geometry.up.x, r*geometry.up.y, r*geometry.up.z, 0};
    RhiImageDesc desc;
    rhi_image_get_desc(world, &desc);
    const RhiViewport viewport = {0, 0, (float)desc.width, (float)desc.height, 0, 1};
    RhiOutputState previous;
    rhi_output_save(&previous);
    rhi_set_viewports(1, &viewport);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLE_STRIP);
    rhi_set_vertex_layout(NULL);
    rhi_set_shader(RHI_STAGE_VERTEX, scope.vertex);
    rhi_set_shader(RHI_STAGE_PIXEL, scope.pixel);
    rhi_set_blend_state(scope.blend, (float[4]){1,1,1,1}, UINT32_MAX);
    rhi_set_depth_state(scope.depth, 0);
    rhi_set_raster_state(scope.raster);
    rhi_buffer_update(scope.constants, &constants);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, SCOPE_UBO, 1, &scope.constants);
    rhi_set_textures(SCOPE_TEXTURE, 1, &image);
    rhi_set_samplers(SCOPE_TEXTURE, 1, &scope.sampler);
    rhi_draw(4, 0);
#ifndef TSFP_RELEASE
    static int reported;
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[XR] Submitted a tracked sniper lens with a shared mono image\n");
    }
#endif
    rhi_set_textures(SCOPE_TEXTURE, 1, (RhiView *[]){NULL});
    rhi_set_samplers(SCOPE_TEXTURE, 1, (RhiSampler *[]){NULL});
    rhi_output_restore(&previous);
}
