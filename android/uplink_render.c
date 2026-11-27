#include "uplink_render.h"
#include "grenade_preview.h"
#include "stereo_backend.h"
#include "xr.h"
#include "xr_linear.h"

#include <stdio.h>
#include <string.h>

enum { UPLINK_CONSTANTS_SLOT = 7 };

static struct {
    RhiShader *vertex, *marker;
    RhiBuffer *constants;
    RhiBlendState *blend;
    RhiDepthState *depth;
    RhiRasterState *raster;
    int attempted;
} renderer;

typedef struct {
    XrVector4f bounds[2], marks[2];
    XrVector4f scale, color;
} UplinkConstants;

static const char shader[] =
    "cbuffer Uplink : register(b7) { float4 bounds[2]; float4 marks[2]; float4 scale; float4 tint; };\n"
    "struct Input { uint id : SV_VertexID; uint eye : TSFPVIEWINDEX0; };\n"
    "struct Output { float4 pos : SV_Position; nointerpolation uint eye : TEXCOORD0; };\n"
    "Output vs_main(Input i) {\n"
    "  Output o; o.eye = i.eye & 1u; float4 box = bounds[o.eye];\n"
    "  float2 uv = lerp(box.xy, box.zw, float2(i.id & 1u, i.id >> 1u));\n"
    "  o.pos = float4(uv * scale.xy * float2(2,-2) + float2(-1,1), 0, 1); return o;\n"
    "}\n"
    "float4 ps_marker(Output i) : SV_Target {\n"
    "  float4 m = marks[i.eye]; float2 q = abs(i.pos.xy - m.xy);\n"
    "  float ring = abs((q.x+q.y)-m.z);\n"
    "  float hit = m.w > 0 ? saturate(2-ring) : saturate(m.z-length(q));\n"
    "  return float4(tint.rgb, hit * tint.a);\n"
    "}\n";

static void destroy_uplink_renderer(void)
{
    if (renderer.vertex) rhi_shader_destroy(renderer.vertex);
    if (renderer.marker) rhi_shader_destroy(renderer.marker);
    if (renderer.constants) rhi_buffer_destroy(renderer.constants);
    if (renderer.blend) rhi_blend_state_destroy(renderer.blend);
    if (renderer.depth) rhi_depth_state_destroy(renderer.depth);
    if (renderer.raster) rhi_raster_state_destroy(renderer.raster);
    memset(&renderer, 0, sizeof renderer);
}

void tsfp_uplink_render_destroy(void)
{
    tsfp_grenade_render_destroy();
    destroy_uplink_renderer();
}

static int initialize(void)
{
    if (renderer.attempted)
        return renderer.constants != NULL;
    renderer.attempted = 1;
    char error[1024] = {0};
    RhiShaderSource source = { .hlsl = shader, .len = sizeof shader - 1, .name = "XR Uplink",
                               .entry = "vs_main", .target = "vs_4_0", .optimize = 1 };
    renderer.vertex = rhi_shader_create(RHI_STAGE_VERTEX, &source, error, sizeof error);
    if (!renderer.vertex)
        goto fail;
    source.target = "ps_4_0";
    source.entry = "ps_marker";
    renderer.marker = rhi_shader_create(RHI_STAGE_PIXEL, &source, error, sizeof error);
    if (!renderer.marker)
        goto fail;
    const RhiBufferDesc constants = { .size = sizeof(UplinkConstants), .usage = RHI_USAGE_DYNAMIC,
                                      .bind = RHI_BIND_UNIFORM, .cpu_access = RHI_CPU_WRITE };
    const RhiBlendDesc blend = { .enable = 1, .src = RHI_BLEND_SRC_ALPHA, .dst = RHI_BLEND_INV_SRC_ALPHA,
        .op = RHI_BLEND_OP_ADD, .src_alpha = RHI_BLEND_ONE, .dst_alpha = RHI_BLEND_INV_SRC_ALPHA,
        .op_alpha = RHI_BLEND_OP_ADD, .write_mask = RHI_WRITE_ALL };
    const RhiDepthDesc depth = {0};
    const RhiRasterDesc raster = { .fill = RHI_FILL_SOLID, .cull = RHI_CULL_NONE, .depth_clip = 1 };
    renderer.blend = rhi_blend_state_create(&blend);
    renderer.depth = rhi_depth_state_create(&depth);
    renderer.raster = rhi_raster_state_create(&raster);
    if (!renderer.blend || !renderer.depth || !renderer.raster)
        goto fail;
    renderer.constants = rhi_buffer_create(&constants, NULL);
    if (renderer.constants)
        return 1;

fail:
    fprintf(stderr, "[6DoF] Could not initialize the Uplink effect renderer %s\n", error);
    destroy_uplink_renderer();
    renderer.attempted = 1;
    return 0;
}

static XrVector4f eye_point(const float rows[16], XrVector3f p)
{
    float v[4];
    for (unsigned i = 0; i < 4; ++i)
        v[i] = rows[i*4] * p.x + rows[i*4+1] * p.y + rows[i*4+2] * p.z + rows[i*4+3];
    return (XrVector4f){v[0],v[1],v[2],v[3]};
}

static XrVector2f pixels(XrVector4f p, float width, float height)
{
    return (XrVector2f){ (.5f + .5f * p.x / p.w) * width, (.5f - .5f * p.y / p.w) * height };
}

static XrVector4f bounds(XrVector2f a, XrVector2f b, float pad, float width, float height)
{
    return (XrVector4f){
        fminf(width, fmaxf(0, floorf(fminf(a.x,b.x)-pad))),
        fminf(height, fmaxf(0, floorf(fminf(a.y,b.y)-pad))),
        fmaxf(0, fminf(width, ceilf(fmaxf(a.x,b.x)+pad))),
        fmaxf(0, fminf(height, ceilf(fmaxf(a.y,b.y)+pad)))
    };
}

unsigned tsfp_uplink_project_beam(const TsfpUplinkVisual *visual, float width, float height,
                                  XrVector4f segments[2])
{
    float eyes[32];
    if (!visual->beam || !tsfp_vk_uplink_source() || !tsfp_xr_uplink_clip(eyes))
        return 0;
    unsigned mask = 0;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const float *rows = eyes + eye * 16u;
        XrVector4f start = eye_point(rows, visual->start);
        XrVector4f end = eye_point(rows, visual->end);
        if (!(end.w > .04f))
            continue;
        if (start.w < .04f) {
            const float t = (.04f - start.w) / (end.w - start.w);
            start.x += (end.x - start.x) * t;
            start.y += (end.y - start.y) * t;
            start.w = .04f;
        }
        const XrVector2f a = pixels(start, width, height), b = pixels(end, width, height);
        if (!isfinite(a.x) || !isfinite(a.y) || !isfinite(b.x) || !isfinite(b.y))
            continue;
        if (fmaxf(a.x, b.x) < -width || fminf(a.x, b.x) > 2 * width ||
            fmaxf(a.y, b.y) < -height || fminf(a.y, b.y) > 2 * height)
            continue;
        // Bound coordinates before the title converts its rectangle to Xbox
        // fixed point. The title clips the animated strips against its viewport.
        const XrVector4f segment = {fminf(fmaxf(a.x, -width), 2 * width),
                                    fminf(fmaxf(a.y, -height), 2 * height),
                                    fminf(fmaxf(b.x, -width), 2 * width),
                                    fminf(fmaxf(b.y, -height), 2 * height)};
        if (fabsf(segment.z - segment.x) + fabsf(segment.w - segment.y) < .01f)
            continue;
        segments[eye] = segment;
        mask |= 1u << eye;
    }
    return mask;
}

void tsfp_uplink_render_marker(const TsfpUplinkVisual *visual)
{
    RhiImage *source = tsfp_vk_uplink_source();
    float eyes[32];
    if (!source || !tsfp_xr_uplink_clip(eyes) || !initialize())
        return;
    RhiImageDesc scene;
    rhi_image_get_desc(source, &scene);
    const float width = (float)scene.width, height = (float)scene.height;
    UplinkConstants data = { .scale = {1/width, 1/height, 0, 0} };
    const float marker_size = visual->target ? height * .012f : height * .0022f;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const float *rows = eyes + eye * 16u;
        const XrVector4f mark = eye_point(rows, visual->marker);
        if (mark.w > .04f) {
            const XrVector2f p = pixels(mark, width, height);
            data.marks[eye] = (XrVector4f){p.x,p.y,marker_size,visual->target};
            data.bounds[eye] = bounds(p,p,marker_size + 2,width,height);
        }
    }
    const RhiViewport viewport = {0,0,width,height,0,1};
    RhiOutputState previous;
    rhi_output_save(&previous);
    rhi_set_viewports(1, &viewport);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLE_STRIP);
    rhi_set_shader(RHI_STAGE_VERTEX, renderer.vertex);
    const float blend_factor[4] = {1,1,1,1};
    rhi_set_blend_state(renderer.blend, blend_factor, UINT32_MAX);
    rhi_set_depth_state(renderer.depth, 0);
    rhi_set_raster_state(renderer.raster);
    data.color = visual->held ? (XrVector4f){1,.78f,.24f,.95f} : visual->target ?
        (XrVector4f){.30f,1,.48f,.95f} : (XrVector4f){1,1,1,.65f};
    rhi_buffer_update(renderer.constants, &data);
    rhi_set_shader(RHI_STAGE_PIXEL, renderer.marker);
    rhi_set_uniform_buffers(RHI_STAGE_VERTEX, UPLINK_CONSTANTS_SLOT, 1, &renderer.constants);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, UPLINK_CONSTANTS_SLOT, 1, &renderer.constants);
    rhi_draw(4, 0);
    rhi_output_restore(&previous);
}
