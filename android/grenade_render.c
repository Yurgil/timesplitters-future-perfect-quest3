#include "grenade_preview.h"
#include "stereo_backend.h"
#include "xr.h"

#include <stdio.h>
#include <string.h>

enum { GRENADE_CONSTANTS_SLOT = 7 };

typedef struct {
    float eyes[32];
    XrVector4f points[TSFP_GRENADE_MAX_POINTS];
    XrVector4f scale;
} GrenadeConstants;

static struct {
    RhiShader *vertex, *pixel;
    RhiBuffer *constants;
    RhiBlendState *blend;
    RhiDepthState *depth;
    RhiRasterState *raster;
    int attempted;
} renderer;

#define GRENADE_TEXT_INNER(value) #value
#define GRENADE_TEXT(value) GRENADE_TEXT_INNER(value)
static const char shader[] =
    "cbuffer Arc : register(b7) { float4 eyes[8]; float4 points["
    GRENADE_TEXT(TSFP_GRENADE_MAX_POINTS) "]; float4 scale; };\n"
    "struct Input { uint id : SV_VertexID; uint eye : TSFPVIEWINDEX0; };\n"
    "struct Output { float4 pos : SV_Position; float2 q : TEXCOORD0;"
    " nointerpolation uint contact : TEXCOORD1; };\n"
    "float4 project(float4 p, uint r) {\n"
    "  return float4(dot(eyes[r],p),dot(eyes[r+1],p),dot(eyes[r+2],p),dot(eyes[r+3],p));\n"
    "}\n"
    "Output vs_main(Input i) {\n"
    "  uint n = i.id / 6u, v = i.id % 6u, r = (i.eye & 1u) * 4u;\n"
    "  float2 uv = float2(v == 1u || v == 3u || v == 4u, v >= 2u && v != 3u);\n"
    "  Output o; o.pos = float4(0,0,-1,1); o.q = uv*2-1; o.contact = n == (uint)scale.z;\n"
    "  float4 a = project(points[n],r);\n"
    "  if (o.contact) {\n"
    "    if (a.z <= 0 || a.w <= 0) return o;\n"
    "    o.pos = a; o.pos.xy += o.q * scale.xy * (scale.w*3.2) * a.w;\n"
    "    o.pos.z -= min(a.z*.5, a.w*.00012); return o;\n"
    "  }\n"
    "  float4 b = project(points[n+1u],r);\n"
    "  if (a.z <= 0 && b.z <= 0) return o;\n"
    "  if (a.z < 0) a = lerp(a,b,-a.z/(b.z-a.z));\n"
    "  if (b.z < 0) b = lerp(b,a,-b.z/(a.z-b.z));\n"
    "  if (a.w <= 0 || b.w <= 0) return o;\n"
    "  float2 span = (b.xy/b.w - a.xy/a.w)/scale.xy;\n"
    "  if (dot(span,span) < .01) return o;\n"
    "  float2 normal = normalize(float2(-span.y,span.x));\n"
    "  o.pos = lerp(a,b,lerp(n == 0u ? 0 : .08,.88,uv.x));\n"
    "  o.pos.xy += normal * o.q.y * scale.xy * scale.w * o.pos.w;\n"
    "  return o;\n"
    "}\n"
    "float4 ps_main(Output i) : SV_Target {\n"
    "  float edge = i.contact ? abs(length(i.q)-.70)*2.8 : max(abs(i.q.y), abs(i.q.x)*.78);\n"
    "  float core = 1-smoothstep(.18,.58,edge);\n"
    "  float halo = 1-smoothstep(.46,1,edge);\n"
    "  return float4(lerp(float3(.05,.70,.66),float3(.72,1,.94),core),(.30*halo+.55*core));\n"
    "}\n";
#undef GRENADE_TEXT
#undef GRENADE_TEXT_INNER

void tsfp_grenade_render_destroy(void)
{
    if (renderer.vertex) rhi_shader_destroy(renderer.vertex);
    if (renderer.pixel) rhi_shader_destroy(renderer.pixel);
    if (renderer.constants) rhi_buffer_destroy(renderer.constants);
    if (renderer.blend) rhi_blend_state_destroy(renderer.blend);
    if (renderer.depth) rhi_depth_state_destroy(renderer.depth);
    if (renderer.raster) rhi_raster_state_destroy(renderer.raster);
    memset(&renderer, 0, sizeof renderer);
}

static int initialize(void)
{
    if (renderer.attempted)
        return renderer.constants != NULL;
    renderer.attempted = 1;
    char error[1024] = {0};
    RhiShaderSource source = { .hlsl = shader, .len = sizeof shader - 1, .name = "XR Grenade Arc",
        .entry = "vs_main", .target = "vs_4_0", .optimize = 1 };
    renderer.vertex = rhi_shader_create(RHI_STAGE_VERTEX, &source, error, sizeof error);
    if (!renderer.vertex)
        goto fail;
    source.target = "ps_4_0";
    source.entry = "ps_main";
    renderer.pixel = rhi_shader_create(RHI_STAGE_PIXEL, &source, error, sizeof error);
    if (!renderer.pixel)
        goto fail;
    const RhiBufferDesc constants = { .size = sizeof(GrenadeConstants), .usage = RHI_USAGE_DYNAMIC,
                                      .bind = RHI_BIND_UNIFORM, .cpu_access = RHI_CPU_WRITE };
    const RhiBlendDesc blend = { .enable = 1, .src = RHI_BLEND_SRC_ALPHA, .dst = RHI_BLEND_INV_SRC_ALPHA,
        .op = RHI_BLEND_OP_ADD, .src_alpha = RHI_BLEND_ONE, .dst_alpha = RHI_BLEND_INV_SRC_ALPHA,
        .op_alpha = RHI_BLEND_OP_ADD, .write_mask = RHI_WRITE_ALL };
    const RhiDepthDesc depth = {.depth_enable = 1, .depth_func = RHI_CMP_LESS_EQUAL};
    const RhiRasterDesc raster = {.fill = RHI_FILL_SOLID, .cull = RHI_CULL_NONE, .depth_clip = 1};
    renderer.blend = rhi_blend_state_create(&blend);
    renderer.depth = rhi_depth_state_create(&depth);
    renderer.raster = rhi_raster_state_create(&raster);
    if (!renderer.blend || !renderer.depth || !renderer.raster)
        goto fail;
    renderer.constants = rhi_buffer_create(&constants, NULL);
    if (renderer.constants)
        return 1;
fail:
    fprintf(stderr, "[XR] Could not initialize grenade arc renderer %s\n", error);
    tsfp_grenade_render_destroy();
    renderer.attempted = 1;
    return 0;
}

void tsfp_grenade_render(const TsfpGrenadeArc *arc, const XrMatrix4x4f *view)
{
    if (arc->count < 2 || !tsfp_vk_uplink_world_bound())
        return;
    GrenadeConstants data = {0};
    RhiImage *world = tsfp_vk_uplink_source();
    if (!world || !tsfp_xr_uplink_clip(data.eyes) || !initialize())
        return;
    for (unsigned i = 0; i < arc->count; ++i) {
        XrVector3f p;
        XrMatrix4x4f_TransformVector3f(&p, view, &arc->points[i]);
        if (!tsfp_grenade_vector_valid(p))
            return;
        data.points[i] = (XrVector4f){p.x, p.y, p.z, 1};
    }
    RhiImageDesc desc;
    rhi_image_get_desc(world, &desc);
    const unsigned segments = arc->count - 1;
    data.scale = (XrVector4f){2.0f/desc.width, 2.0f/desc.height, (float)segments,
                             fmaxf(2.0f, desc.height * .0028f)};
    const RhiViewport viewport = {0, 0, (float)desc.width, (float)desc.height, 0, 1};
    RhiOutputState previous;
    rhi_output_save(&previous);
    rhi_set_viewports(1, &viewport);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, renderer.vertex);
    rhi_set_shader(RHI_STAGE_PIXEL, renderer.pixel);
    rhi_set_blend_state(renderer.blend, (float[4]){1,1,1,1}, UINT32_MAX);
    rhi_set_depth_state(renderer.depth, 0);
    rhi_set_raster_state(renderer.raster);
    rhi_buffer_update(renderer.constants, &data);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, GRENADE_CONSTANTS_SLOT, 1, &renderer.constants);
    rhi_draw(6u * (segments + (unsigned)arc->contact), 0);
    rhi_output_restore(&previous);
}
