#include "recomp/gen/recomp_funcs.h"
#ifndef TSFP_RELEASE
#include "hitch_trace.h"
#endif
#include "stereo_backend.h"
#include "stereo_probe.h"
#include "uplink_render.h"
#include <string.h>

extern void tsfp_stereo_redirect_projectile(uint32_t stack, uint32_t hand_record);
extern int tsfp_stereo_flash_begin(uint32_t stack);
extern void tsfp_stereo_flash_end(int role);
extern void tsfp_stereo_flash_position(uint32_t record, uint32_t output, int shot_effect);
extern int tsfp_stereo_immersive_player(void);
extern int tsfp_stereo_hide_reticle(void);
extern void tsfp_stereo_overlay(int begin);
extern void tsfp_stereo_screen_effects(int begin);
extern void tsfp_stereo_first_person_weapon_pass(int begin);
extern int tsfp_xr_world_active(void);
extern void *tsfp_stereo_begin_visibility(float saved[16], int objects);
extern int tsfp_stereo_object_outside(uint32_t entity, const float minimum[3], const float maximum[3]);
extern int tsfp_stereo_uplink_begin(uint32_t state);
extern void tsfp_stereo_uplink_end(void);
extern uint32_t tsfp_stereo_uplink_preview_begin(void);
extern uint32_t tsfp_stereo_uplink_preview_target(uint32_t player);
extern void tsfp_stereo_uplink_preview_end(uint32_t target);
extern int tsfp_stereo_uplink_visual(TsfpUplinkVisual *visual);

static _Thread_local int object_visibility;
#ifndef TSFP_RELEASE
static _Thread_local unsigned objects_tested, objects_culled;
#endif

static void flush_guest_draws(void)
{
    const uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    const uint32_t stack = g_esp, seh = g_seh_ebp;
    const tsfp_status_flags flags = g_status_flags;
    PUSH32(g_esp, 0);
    sub_00022FF0();
    g_esp = stack;
    g_seh_ebp = seh;
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
    g_status_flags = flags;
}

void tsfp_quest_projectile(void)
{
    tsfp_stereo_redirect_projectile(g_esp, g_ebx);
    sub_001B7680();
}

void tsfp_quest_muzzle_flash(void)
{
    const int role = tsfp_stereo_flash_begin(g_esp);
    sub_001B8C70();
    tsfp_stereo_flash_end(role);
}

void tsfp_quest_muzzle_flash_position(void)
{
    const uint32_t record = MEM32(g_esp + 4u), output = MEM32(g_esp + 8u);
    sub_001B8530();
    tsfp_stereo_flash_position(record, output, 0);
}

void tsfp_quest_shot_effect_position(void)
{
    /* The firing routine looks up the world origin for the muzzle sound
     * and flash separately from the projectile. */
    const uint32_t record = MEM32(g_esp + 4u), output = MEM32(g_esp + 8u);
    sub_001B8530();
    tsfp_stereo_flash_position(record, output, 1);
}

void tsfp_quest_weapon_particles(void)
{
    const uint32_t emitter = MEM32(g_esp + 4u);
    const TsfpModelRole role = tsfp_stereo_weapon_particles(emitter);
    if (role == TSFP_MODEL_DEFAULT) {
        sub_00111FC0();
    } else {
        /* 110D30 projects a 2D screen-space flare via the guest camera matrix
         * instead of the stereo weapon view matrix. Suppress it while drawing
         * the tracked 3D particles and restore the definition field afterwards. */
        const uint32_t def = emitter ? MEM32(emitter + 0x0cu) : 0;
        const uint32_t saved_flare = def ? MEM32(def + 0x50u) : 0;
        if (saved_flare)
            MEM32(def + 0x50u) = 0;
        flush_guest_draws();
        /* The particle renderer uses immediate draws and queued sprites. */
        const TsfpModelRole previous = tsfp_stereo_set_model_role(role);
        sub_00111FC0();
        flush_guest_draws();
        tsfp_stereo_set_model_role(previous);
        if (saved_flare)
            MEM32(def + 0x50u) = saved_flare;
    }
}

void tsfp_quest_legacy_reticle(void)
{
    if (tsfp_stereo_hide_reticle())
        g_esp += sizeof(uint32_t);
    else
        sub_0014CC40();
}

static void draw_hud(void (*draw)(void))
{
    if (!tsfp_xr_world_active()) {
        draw();
        return;
    }
    /* The banner queues glyphs in the guest sprite batch. Submit them
     * before restoring the world eye transform. */
    flush_guest_draws();
    tsfp_stereo_overlay(1);
    draw();
    flush_guest_draws();
    tsfp_stereo_overlay(0);
}

void tsfp_quest_hud(void)
{
    draw_hud(sub_0014FF20);
}

void tsfp_quest_objective_notice(void)
{
    /* 14AAE0 draws objective banners from the type-6 notice queue. */
    draw_hud(sub_0014AAE0);
}

static void draw_screen_effects(void (*draw)(void), uint32_t routine)
{
    if (!tsfp_vk_stereo_geometry()) {
        draw();
        return;
    }
    flush_guest_draws();
    tsfp_stereo_screen_effects((int)routine);
    draw();
    flush_guest_draws();
    tsfp_stereo_screen_effects(0);
}

void tsfp_quest_screen_effects(void)
{
    draw_screen_effects(sub_000FE990, 0x000FE990u);
}

void tsfp_quest_story_screen_effects(void)
{
    draw_screen_effects(sub_0006F150, 0x0006F150u);
}

void tsfp_quest_world_screen_effects(void)
{
    draw_screen_effects(sub_000FECB0, 0x000FECB0u);
}

void tsfp_quest_late_screen_effects(void)
{
    draw_screen_effects(sub_000FED20, 0x000FED20u);
}

static void preview_uplink_target(void)
{
    const uint32_t player = tsfp_stereo_uplink_preview_begin();
    if (!player)
        return;
    const uint32_t stack = g_esp, seh = g_seh_ebp;
    const uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    const tsfp_status_flags flags = g_status_flags;
    const RecompXmm sse[] = {g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7};
    const int fp_top = g_fp_top, fp_cmp = g_fp_cmp;
    const uint16_t fp_cc = g_fp_cc;
    double fp_stack[8];
    memcpy(fp_stack, g_fp_stack, sizeof fp_stack);
    PUSH32(g_esp, player);
    PUSH32(g_esp, 0);
    sub_003501E0();
    const uint32_t entity = g_eax ? tsfp_stereo_uplink_preview_target(player) : 0;
    uint32_t handle = 0;
    if (entity) {
        PUSH32(g_esp, 0x42480000u);
        PUSH32(g_esp, 0);
        PUSH32(g_esp, 0);
        PUSH32(g_esp, entity);
        PUSH32(g_esp, 0);
        sub_0025B7B0();
        if (g_eax)
            handle = MEM32(entity + 0x2cu);
    }
    g_esp = stack;
    g_seh_ebp = seh;
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    g_status_flags = flags;
    g_xmm0 = sse[0]; g_xmm1 = sse[1]; g_xmm2 = sse[2]; g_xmm3 = sse[3];
    g_xmm4 = sse[4]; g_xmm5 = sse[5]; g_xmm6 = sse[6]; g_xmm7 = sse[7];
    g_fp_top = fp_top; g_fp_cmp = fp_cmp; g_fp_cc = fp_cc;
    memcpy(g_fp_stack, fp_stack, sizeof fp_stack);
    tsfp_stereo_uplink_preview_end(handle);
}

static void draw_uplink_beam(const TsfpUplinkVisual *visual)
{
    if (!visual->beam)
        return;
    const uint32_t view = MEM32(0x007E71C8u);
    if (!view)
        return;
    const int32_t left = MEM32(view), top = MEM32(view + 0x14u);
    const int32_t width = (int32_t)MEM32(view + 4u) - left + 1;
    const int32_t height = (int32_t)MEM32(view + 0x18u) - top + 1;
    if (width <= 0 || height <= 0)
        return;
    XrVector4f segments[2];
    const unsigned eyes = tsfp_uplink_project_beam(visual, (float)width, (float)height, segments);
    if (!eyes)
        return;

    const uint32_t stack = g_esp, seh = g_seh_ebp;
    const uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    const tsfp_status_flags flags = g_status_flags;
    const RecompXmm sse[] = {g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7};
    const int fp_top = g_fp_top, fp_cmp = g_fp_cmp;
    const uint16_t fp_cc = g_fp_cc;
    double fp_stack[8];
    memcpy(fp_stack, g_fp_stack, sizeof fp_stack);

    for (unsigned eye = 0; eye < 2; ++eye) {
        const unsigned mask = 1u << eye;
        if (!(eyes & mask))
            continue;
        const XrVector4f segment = segments[eye];
        const float xy[] = {segment.x + left, segment.y + top, segment.z + left, segment.w + top};
        uint32_t beam[15] = {0};
        /* +8 stays zero so the original draw uses its cached pixel endpoints
         * instead of projecting the Xbox camera's world endpoints again. */
        memcpy(beam + 9, xy, sizeof xy);
        memcpy(beam + 13, &visual->age, sizeof visual->age);
        memcpy(beam + 14, &visual->fade, sizeof visual->fade);
        g_esp = stack - sizeof beam;
        for (unsigned i = 0; i < sizeof beam / sizeof beam[0]; ++i)
            MEM32(g_esp + i * sizeof beam[0]) = beam[i];
        const uint32_t record = g_esp;
        tsfp_vk_uplink_eye(mask);
        PUSH32(g_esp, record);
        PUSH32(g_esp, 0);
        sub_00347FE0();
        g_esp = stack;
        flush_guest_draws();
    }
    tsfp_vk_uplink_eye(0);
    g_seh_ebp = seh;
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    g_status_flags = flags;
    g_xmm0 = sse[0]; g_xmm1 = sse[1]; g_xmm2 = sse[2]; g_xmm3 = sse[3];
    g_xmm4 = sse[4]; g_xmm5 = sse[5]; g_xmm6 = sse[6]; g_xmm7 = sse[7];
    g_fp_top = fp_top; g_fp_cmp = fp_cmp; g_fp_cc = fp_cc;
    memcpy(g_fp_stack, fp_stack, sizeof fp_stack);
}

void tsfp_quest_first_person_weapon_pass(void)
{
    const int immersive = tsfp_stereo_immersive_player();
    if (immersive) {
        tsfp_vk_shield_capture(1);
        tsfp_stereo_first_person_weapon_pass(1);
    }
    sub_001B9120();
    if (immersive) {
        tsfp_stereo_first_person_weapon_pass(0);
        tsfp_vk_shield_capture(2);
        preview_uplink_target();
        TsfpUplinkVisual visual;
        if (tsfp_stereo_uplink_visual(&visual)) {
            flush_guest_draws();
            draw_uplink_beam(&visual);
            tsfp_uplink_render_marker(&visual);
        }
    }
}

void tsfp_quest_uplink_grab(void)
{
    const int tracked = tsfp_stereo_uplink_begin(MEM32(g_esp + 4u));
    sub_003508B0();
    if (tracked)
        tsfp_stereo_uplink_end();
}

int tsfp_quest_uplink_physics_begin(uint32_t callback)
{
    /* Rigid bodies and articulated actors have separate spring/throw callbacks. */
    return (callback == 0x0034F5B0u || callback == 0x00350320u) &&
        tsfp_stereo_uplink_physics_begin(MEM32(g_esp + 4u));
}

void tsfp_quest_uplink_physics_end(void)
{
    tsfp_stereo_uplink_physics_end();
}

void tsfp_quest_uplink_map(void)
{
    if (!tsfp_stereo_prepare_uplink_map(g_edi)) {
        sub_0034CAA0();
        return;
    }
    flush_guest_draws();
    const TsfpModelRole previous = tsfp_stereo_set_model_role(TSFP_MODEL_UPLINK_MAP);
    sub_0034CAA0();
    flush_guest_draws();
    tsfp_stereo_set_model_role(previous);
}

void tsfp_quest_uplink_legacy_beams(void)
{
    if (tsfp_xr_world_active()) {
        tsfp_vk_shield_capture(0);
        g_esp += sizeof(uint32_t);
    } else {
        sub_003492D0();
    }
}

static void draw_visibility(void (*original)(void), const char *pass, unsigned *samples, int objects)
{
    float saved[16];
    void *projection = tsfp_stereo_begin_visibility(saved, objects);
#ifndef TSFP_RELEASE
    const int sample = projection && (*samples)++ % 64u == 0;
    const int64_t start = sample ? tsfp_hitch_now_ns() : 0;
#else
    (void)pass;
    (void)samples;
#endif
    const int previous = object_visibility;
    object_visibility = projection && objects;
    original();
    object_visibility = previous;
    if (projection) {
        memcpy(projection, saved, sizeof saved);
#ifndef TSFP_RELEASE
        if (sample)
            TSFP_HITCH_END(start, 15, "XR %s visibility", pass);
        if (objects && *samples % 120u == 0) {
            __android_log_print(ANDROID_LOG_INFO, "TSFP-Hitch", "XR objects culled in both eyes: %u/%u",
                                objects_culled, objects_tested);
            objects_culled = objects_tested = 0;
        }
#endif
    }
}

void tsfp_quest_portal_visibility(void)
{
    static _Thread_local unsigned samples;
    draw_visibility(sub_00087F80, "portal", &samples, 0);
}

void tsfp_quest_object_visibility(void)
{
    static _Thread_local unsigned samples;
    draw_visibility(sub_000A79B0, "object", &samples, 1);
}

void tsfp_quest_object_occlusion(void)
{
    if (object_visibility) {
        /* A2430 supplies the two opposite corners from A1FA0's world AABB. */
        const uint32_t low = MEM32(g_esp + 4u), high = MEM32(g_esp + 8u);
        if (low == g_esp + 0x30u && high == low + 0x70u) {
            const float minimum[3] = { MEMF(low), MEMF(low + 4u), MEMF(low + 8u) };
            const float maximum[3] = { MEMF(high), MEMF(high + 4u), MEMF(high + 8u) };
#ifndef TSFP_RELEASE
            ++objects_tested;
#endif
            if (tsfp_stereo_object_outside(MEM32(g_esp + 0x134u), minimum, maximum)) {
#ifndef TSFP_RELEASE
                ++objects_culled;
#endif
                g_esp += sizeof(uint32_t);
                g_eax = UINT32_MAX;
                return;
            }
        }
    }
    sub_0008C020();
}
