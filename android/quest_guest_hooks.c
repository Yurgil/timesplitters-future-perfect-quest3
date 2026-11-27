#include "recomp/gen/recomp_funcs.h"
#ifndef TSFP_RELEASE
#include "hitch_trace.h"
#endif
#include "grenade_preview.h"
#include "guest_virtual_memory.h"
#include "stereo_backend.h"
#include "xbox_memory_layout.h"
#include "stereo_probe.h"
#include "uplink_render.h"
#include <string.h>
#ifdef TSFP_CART_TEST
#include "catacombs_cart.h"
#include <android/log.h>
#endif

extern int tsfp_xr_geometry_enabled(void);
extern void tsfp_stereo_redirect_projectile(uint32_t stack, uint32_t hand_record);
extern void tsfp_stereo_cart_actor_projectile(uint32_t stack, void (*fire)(void));
extern int tsfp_stereo_flash_begin(uint32_t stack);
extern void tsfp_stereo_flash_end(int role);
extern void tsfp_stereo_flash_position(uint32_t record, uint32_t output, int shot_effect);
extern int tsfp_stereo_immersive_player(void);
extern int tsfp_stereo_hide_reticle(void);
extern void tsfp_stereo_overlay(int begin);
extern void tsfp_stereo_screen_effects(int begin);
extern void tsfp_stereo_first_person_weapon_pass(int begin);
extern int tsfp_xr_world_active(void);
#ifndef TSFP_RELEASE
extern void tsfp_stereo_trace_projectile(uint32_t stack);
#endif
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
#ifndef TSFP_RELEASE
    tsfp_stereo_trace_projectile(g_esp);
#endif
    tsfp_stereo_redirect_projectile(g_esp, g_ebx);
    sub_001B7680();
}

void tsfp_quest_cart_actor_projectile(void)
{
#ifndef TSFP_RELEASE
    tsfp_stereo_trace_projectile(g_esp);
#endif
    tsfp_stereo_cart_actor_projectile(g_esp, sub_001B7680);
}

#ifndef TSFP_RELEASE
void tsfp_quest_trace_projectile(void)
{
    tsfp_stereo_trace_projectile(g_esp);
    sub_001B7680();
}
#endif

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

static void right_stick_aim(uint32_t player, uint32_t mode, void (*update)(void))
{
    const uint32_t offsets[] = {0x58u, 0x5cu, 0x68u, 0x6cu};
    const uint32_t axes[] = {4u, 3u, 4u, 3u}; /* Xbox RY, RX */
    /* The scripted cockpit reads only the first pitch/yaw pair. */
    const unsigned count = mode == TSFP_CAMERA_SCRIPTED ? 2 : 4;
    uint32_t saved[4];
    for (unsigned i = 0; i < count; ++i) {
        saved[i] = MEM32(player + offsets[i]);
        MEM32(player + offsets[i]) = axes[i];
    }
#ifndef TSFP_RELEASE
    static uint32_t reported_modes;
    if (!(reported_modes & mode)) {
        fprintf(stderr, "[%s] Mode %x Xbox pitch/yaw bindings routed to RY/RX\n",
                tsfp_turret_uses_aim_bindings(mode) ? "XR-TURRET" : "XR-MECH", mode);
        reported_modes |= mode;
    }
#endif
    update();
    for (unsigned i = 0; i < count; ++i)
        MEM32(player + offsets[i]) = saved[i];
}

void tsfp_quest_turret_look(void)
{
    const uint32_t player = MEM32(0x007B0C7Cu);
    const uint32_t mode = player ? MEM32(player + 0x114u) : 0;
    /* 25CE80 supplies both turret aim and the look vector consumed by
     * the Battle Mech update at 37AA70. It can use either binding pair. */
    if ((tsfp_turret_uses_aim_bindings(mode) || mode == TSFP_CAMERA_BATTLE_MECH) &&
        MEM32(player + 4u) == 0 && tsfp_xr_geometry_enabled()) {
        right_stick_aim(player, mode, sub_0025CE80);
        return;
    }
    sub_0025CE80();
}

void tsfp_quest_vehicle_update(void)
{
    const uint32_t player = MEM32(0x007B0C7Cu);
    /* The scripted update calls 21C900, which reads aim axes itself. */
    if (player && MEM32(player + 0x114u) == TSFP_CAMERA_SCRIPTED &&
        tsfp_xr_geometry_enabled() && tsfp_stereo_vehicle_look() == TSFP_VEHICLE_LOOK_MECH) {
        right_stick_aim(player, TSFP_CAMERA_SCRIPTED, sub_000D5C70);
        return;
    }
    tsfp_stereo_cart_update(sub_000D5C70);
}

void tsfp_quest_cart_visible(void)
{
    const uint32_t entity = MEM32(g_esp + 4u);
    const uint32_t camera = MEM32(0x007B0CC0u);
    const uint32_t view = MEM32(g_esp) == 0x000A2AB8u && camera ?
        MEM32(camera + 0x214u) : MEM32(g_esp + 8u);
    sub_000A1AB0();
    /* The portal plane result is still used for all other entities and
     * views. The stereo eye frusta clip the local rider and attachments. */
    if (!g_eax && tsfp_stereo_cart_visible(entity, view))
        g_eax = 1;
}

static void draw_aim_panel(void (*draw)(void), TsfpReticleDraw role, uint32_t player)
{
    flush_guest_draws();
    uint32_t saved_x = 0, saved_y = 0;
    if (player) {
        /* The guest subtracts these offsets from the viewport center. */
        saved_x = MEM32(player + 0xf6cu);
        saved_y = MEM32(player + 0xf70u);
        MEM32(player + 0xf6cu) = MEM32(player + 0xf70u) = 0;
    }
    tsfp_stereo_turret_reticle(role);
    draw();
    if (player) {
        MEM32(player + 0xf6cu) = saved_x;
        MEM32(player + 0xf70u) = saved_y;
    }
    flush_guest_draws();
    tsfp_stereo_turret_reticle(TSFP_RETICLE_NONE);
}

static void draw_mounted_reticle(void (*draw)(void))
{
    if (tsfp_xr_world_active() && tsfp_stereo_mounted_gun())
        draw_aim_panel(draw, TSFP_RETICLE_TURRET, MEM32(0x007B0C7Cu));
    else
        draw();
}

void tsfp_quest_mech_reticle(void)
{
    const uint32_t player = MEM32(0x007B0C7Cu);
    if (tsfp_xr_world_active() && player && MEM32(player + 4u) == 0) {
        const uint32_t mode = MEM32(player + 0x114u);
        const uint32_t hud = MEM32(0x007A6160u);
        /* 139010 selects 37A9D0 (Battle Mech) or 145E40 (final cockpit).
         * The Battle Mech shares its muzzle and direction with the camera. */
        if (mode == TSFP_CAMERA_BATTLE_MECH && hud == 0x24u) {
            draw_aim_panel(sub_00139010, TSFP_RETICLE_MECH, player);
            return;
        }
        if (mode == TSFP_CAMERA_SCRIPTED && hud == 0x26u &&
            tsfp_stereo_vehicle_look() == TSFP_VEHICLE_LOOK_MECH) {
            draw_aim_panel(sub_00139010, TSFP_RETICLE_MECH, 0);
            return;
        }
    }
    sub_00139010();
}

void tsfp_quest_legacy_reticle(void)
{
    if (tsfp_stereo_hide_reticle())
        g_esp += sizeof(uint32_t);
    else
        draw_mounted_reticle(sub_0014CC40);
}

void tsfp_quest_alternate_reticle(void)
{
    draw_mounted_reticle(sub_0014C970);
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

void tsfp_quest_chapter_title(void (*draw)(void))
{
    /* 13FC50 draws the story date and chapter name, with their outlines. */
    draw_hud(draw);
}

static int full_guest_viewport(uint32_t *width, uint32_t *height)
{
    if (!tsfp_vk_stereo_geometry())
        return 0;
    const uint32_t view = MEM32(0x007E71C8u);
    *width = MEM32(0x004E8CF0u);
    *height = MEM32(0x004E8CF4u);
    if (!view || !*width || !*height || MEM32(view) || MEM32(view + 0x14u))
        return 0;
    const uint32_t right = MEM32(view + 4u), bottom = MEM32(view + 0x18u);
    return (right == *width - 1 || right == *width) &&
           (bottom == *height - 1 || bottom == *height);
}

static void draw_screen_overlay(void (*draw)(void), TsfpScreenOverlay scope)
{
    flush_guest_draws();
    const TsfpScreenOverlay previous = tsfp_stereo_set_screen_overlay(scope);
    draw();
    flush_guest_draws();
    tsfp_stereo_set_screen_overlay(previous);
}

void tsfp_quest_remote_view_overlay(void (*draw)(void))
{
    uint32_t width, height;
    if (full_guest_viewport(&width, &height))
        draw_screen_overlay(draw, TSFP_SCREEN_OVERLAY_SCENE);
    else
        draw();
}

static int draw_cinematic_matte(uint32_t frame, int top)
{
    if (!tsfp_vk_stereo_geometry())
        return 0;
    const uint32_t color = MEM32(frame + 20u);
    if (color != 0x7fu && color != 0x80u)
        return 0;
    const uint32_t view = MEM32(0x007E71C8u);
    if (!view)
        return 0;
    const int32_t view_top = (int32_t)MEM32(view + 0x14u);
    const int32_t view_bottom = (int32_t)MEM32(view + 0x18u);
    if (view_bottom < view_top)
        return 0;
    /* Keep the authored inner edge with respect to the guest view. The
     * outer edges come from the stereo attachment, including inset views. */
    const int32_t edge = (int32_t)MEM32(frame + (top ? 16u : 8u));
    const float fraction = ((float)edge - view_top) / ((float)view_bottom - view_top + 1.0f);
    flush_guest_draws();
    if (!tsfp_vk_cinematic_matte(MEM32(frame), top, fraction))
        return 0;
    g_esp += sizeof(uint32_t);
    return 1;
}

int tsfp_quest_rect_overlay(void (*draw)(void))
{
    const uint32_t frame = g_esp;
    TsfpScreenOverlay scope = TSFP_SCREEN_OVERLAY_STEREO;
    switch (MEM32(frame)) {
    /* Restore the pre-mission quad's original matte policy. */
    case 0x0006F322u: case 0x0006F34Bu:
        if (!tsfp_xr_geometry_enabled())
            return 0;
        g_esp += sizeof(uint32_t);
        return 1;
    /* Scripted matte, tint transition and the film filter's separate matte. */
    case 0x0013972Eu: case 0x0013BF00u: case 0x0034565Cu:
        return draw_cinematic_matte(frame, 1);
    case 0x0013970Eu: case 0x0013BF2Au: case 0x00345670u:
        return draw_cinematic_matte(frame, 0);
    /* Story fades keep the authored framing on the pre-mission quad. */
    case 0x0006F227u: case 0x0006F264u: case 0x0006F2F0u:
    case 0x0006F381u: case 0x0006F3ABu:
    case 0x000FEA07u: case 0x000FEBC3u:
        break;
    /* These mattes' tints and the in-game transitions also draw over mono. */
    case 0x0013BED7u: case 0x00344F6Au:
    case 0x001CF429u: case 0x001CF5E5u:
        scope = TSFP_SCREEN_OVERLAY_SCENE;
        break;
    default:
        return 0;
    }

    uint32_t width, height;
    if (!full_guest_viewport(&width, &height) || MEM32(frame + 4u))
        return 0;
    const uint32_t right = MEM32(frame + 12u), bottom = MEM32(frame + 16u);
    if (right < width - 1 || right > width + 1)
        return 0;
    /* The guest also uses inclusive edges. Pass the exact canvas edge to
     * the rasterizer and keep the vertical height and animation of the bars. */
    MEM32(frame + 12u) = width;
    if (bottom == height - 1 || bottom == height + 1)
        MEM32(frame + 16u) = height;
    draw_screen_overlay(draw, scope);
    MEM32(frame + 12u) = right;
    MEM32(frame + 16u) = bottom;
    return 1;
}

int tsfp_quest_flash_overlay(void (*draw)(void))
{
    uint32_t width, height;
    if (!full_guest_viewport(&width, &height))
        return 0;
    draw_screen_overlay(draw, TSFP_SCREEN_OVERLAY_STEREO);
    return 1;
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

typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, seh;
    RecompXmm xmm[8];
    double fp_stack[8];
    int fp_top, fp_cmp;
    uint16_t fp_cc;
    tsfp_status_flags flags;
} EffectRegisters;

static EffectRegisters effect_registers(void)
{
    EffectRegisters regs = {
        .eax = g_eax, .ebx = g_ebx, .ecx = g_ecx, .edx = g_edx, .esi = g_esi,
        .edi = g_edi, .ebp = g_ebp, .esp = g_esp, .seh = g_seh_ebp,
        .xmm = {g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7},
        .fp_top = g_fp_top, .fp_cmp = g_fp_cmp, .fp_cc = g_fp_cc, .flags = g_status_flags,
    };
    memcpy(regs.fp_stack, g_fp_stack, sizeof regs.fp_stack);
    return regs;
}

static void restore_effect_registers(const EffectRegisters *regs)
{
    g_eax = regs->eax; g_ebx = regs->ebx; g_ecx = regs->ecx; g_edx = regs->edx;
    g_esi = regs->esi; g_edi = regs->edi; g_ebp = regs->ebp; g_esp = regs->esp;
    g_seh_ebp = regs->seh; g_status_flags = regs->flags;
    g_xmm0 = regs->xmm[0]; g_xmm1 = regs->xmm[1]; g_xmm2 = regs->xmm[2]; g_xmm3 = regs->xmm[3];
    g_xmm4 = regs->xmm[4]; g_xmm5 = regs->xmm[5]; g_xmm6 = regs->xmm[6]; g_xmm7 = regs->xmm[7];
    g_fp_top = regs->fp_top; g_fp_cmp = regs->fp_cmp; g_fp_cc = regs->fp_cc;
    memcpy(g_fp_stack, regs->fp_stack, sizeof regs->fp_stack);
}

#ifdef TSFP_CART_TEST
#include "tests/cart_segment.inc"
#endif

static void set_guest_camera(uint32_t view, uint32_t projection)
{
    const uint32_t stack = g_esp;
    PUSH32(g_esp, view);
    PUSH32(g_esp, 0);
    sub_0001C6F0();
    g_esp = stack;
    PUSH32(g_esp, projection);
    PUSH32(g_esp, 0);
    sub_0001C5D0();
    g_esp = stack;
}

typedef struct {
    uint32_t low, high, calls;
} EffectRandom;

/* Screen effects can use the guest's shared RNG. */
static EffectRandom effect_random(void)
{
    return (EffectRandom){MEM32(0x004E7950u), MEM32(0x004E7954u), MEM32(0x007497C8u)};
}

static void restore_effect_random(EffectRandom random)
{
    MEM32(0x004E7950u) = random.low;
    MEM32(0x004E7954u) = random.high;
    MEM32(0x007497C8u) = random.calls;
}

static void draw_eye_projected_effect(void (*draw)(void), int screen_routine)
{
    TsfpGhostCamera camera;
    if (!tsfp_vk_ghost_ready() || !tsfp_stereo_ghost_camera(&camera)) {
        draw();
        return;
    }

    /* These effects calculate screen bounds on the CPU. Replay their model,
     * scratch/feedback and composite passes with a single eye throughout. */
    const EffectRegisters entry = effect_registers();
    float active_camera[2][16];
    memcpy(active_camera, (const void *)XBOX_PTR(0x0055FA00u), sizeof active_camera);
    flush_guest_draws();
    const int previous_routine = tsfp_stereo_screen_effect_draw();
    TsfpScreenOverlay previous_overlay = TSFP_SCREEN_OVERLAY_NONE;
    EffectRandom random_start = {0}, random_end = {0};
    if (screen_routine) {
        tsfp_stereo_screen_effects(screen_routine);
        previous_overlay = tsfp_stereo_set_screen_overlay(TSFP_SCREEN_OVERLAY_SCENE);
        random_start = random_end = effect_random();
    }
    EffectRegisters result;
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        tsfp_vk_ghost_eye(eye + 1u);
        for (unsigned i = 0; i < TSFP_GHOST_CAMERA_MATRICES; ++i)
            memcpy(camera.matrix[i], camera.eyes[eye][i], sizeof camera.eyes[eye][i]);
        set_guest_camera(camera.address[TSFP_GHOST_CAMERA_VIEW], camera.address[TSFP_GHOST_CAMERA_PROJECTION]);
        restore_effect_registers(&entry);
        if (screen_routine)
            restore_effect_random(random_start);
        draw();
        result = effect_registers();
        flush_guest_draws();
        if (screen_routine) {
            /* A shockwave outside one eye's frustum can consume no random
             * values. Commit the sequence for the eye that drew the effect. */
            const EffectRandom random = effect_random();
            if (random.calls - random_start.calls > random_end.calls - random_start.calls)
                random_end = random;
        }
    }
    tsfp_vk_ghost_eye(0);
    if (screen_routine) {
        restore_effect_random(random_end);
        tsfp_stereo_set_screen_overlay(previous_overlay);
        tsfp_stereo_screen_effects(previous_routine);
    }
    for (unsigned i = 0; i < TSFP_GHOST_CAMERA_MATRICES; ++i)
        memcpy(camera.matrix[i], camera.original[i], sizeof camera.original[i]);

    /* The active D3D matrices can differ from the camera's stored matrices. */
    g_esp = (result.esp - sizeof active_camera) & ~15u;
    memcpy((void *)XBOX_PTR(g_esp), active_camera, sizeof active_camera);
    set_guest_camera(g_esp, g_esp + sizeof active_camera[0]);
    restore_effect_registers(&result);
}

void tsfp_quest_ghost_actor(void)
{
    draw_eye_projected_effect(sub_00121F50, 0);
}

void tsfp_quest_ghost_reveal(void)
{
    draw_eye_projected_effect(sub_00122F10, 0);
}

void tsfp_quest_dispersion_impact(void)
{
    draw_eye_projected_effect(sub_000FA640, 0x000FA640);
}

void tsfp_quest_time_grenade_effect(void)
{
    draw_eye_projected_effect(sub_000FAD90, 0x000FAD90);
}

void tsfp_quest_mag_charger_shot(void)
{
    draw_eye_projected_effect(sub_000FBD70, 0x000FBD70);
}

void tsfp_quest_mag_charger_vision(void)
{
    draw_eye_projected_effect(sub_00344EA0, TSFP_MAG_CHARGER_VISION_ROUTINE);
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

typedef struct {
    XrVector3f start, delta, hit;
    uint32_t room;
    uint32_t contacts[64];
} GrenadeSweepScratch;

typedef struct {
    GrenadeSweepScratch *scratch;
    uint32_t address, category;
    uint32_t *owner_flags, saved_flags;
    EffectRegisters registers;
} GrenadeSweepContext;

static int grenade_sweep(void *opaque, XrVector3f start, XrVector3f end,
                          uint32_t *room, XrVector3f *hit)
{
    GrenadeSweepContext *context = opaque;
    GrenadeSweepScratch *scratch = context->scratch;
    if ((int32_t)*room <= 0 || !tsfp_grenade_vector_valid(start) || !tsfp_grenade_vector_valid(end))
        return -1;
    scratch->start = start;
    XrVector3f_Sub(&scratch->delta, &end, &start);
    if (XrVector3f_Dot(&scratch->delta, &scratch->delta) <= 1e-7f)
        return 0;
    scratch->room = *room;
    memset(scratch->contacts, 0, sizeof scratch->contacts);
    const uint32_t base = context->address;
    /* DCB80's stock sweep. The first result is the nearest contact point.
     * Keep the next room for sweeps that cross a portal. */
    const uint32_t args[] = {
        *room, base + offsetof(GrenadeSweepScratch, start),
        base + offsetof(GrenadeSweepScratch, delta), 0,
        base + offsetof(GrenadeSweepScratch, hit), 0,
        base + offsetof(GrenadeSweepScratch, room), context->category,
        0x02000010u, base + offsetof(GrenadeSweepScratch, contacts),
    };
    g_esp = base;
    for (unsigned i = sizeof args / sizeof args[0]; i > 0; --i)
        PUSH32(g_esp, args[i - 1]);
    PUSH32(g_esp, 0);
    sub_00088C60();
    *room = scratch->room;
    if (g_eax) {
        *hit = scratch->hit;
        return 1;
    }
    return (int32_t)*room > 0 ? 0 : -1;
}

static int begin_grenade_sweeps(GrenadeSweepContext *context, const TsfpGrenadeSource *source,
                               uint32_t kind)
{
    if (!source->shooter || (int32_t)source->room <= 0 || g_esp < 0x2000u)
        return 0;
    const uint32_t address = (g_esp - sizeof(GrenadeSweepScratch)) & ~15u;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    GrenadeSweepScratch *scratch = vm.resolve(vm.host, address, sizeof *scratch, 1);
    uint32_t *owner_flags = vm.resolve(vm.host, source->shooter + 0x28u, sizeof *owner_flags, 1);
    if (!scratch || !owner_flags)
        return 0;
    /* The sticky plasma definition sets bit 0x200, which selects category 4 in DCB80. */
    const uint32_t category = kind == TSFP_GRENADE_PLASMA_STICKY_KIND ? 4u :
        (MEM32(0x00739A90u) == 1u ? 0x20u : 0u);
    *context = (GrenadeSweepContext){
        .scratch = scratch, .address = address, .category = category,
        .owner_flags = owner_flags, .saved_flags = *owner_flags, .registers = effect_registers(),
    };
    /* Match the grenade's first stock sweep, which ignores its owner. */
    *owner_flags &= ~0x60u;
    return 1;
}

static void end_grenade_sweeps(const GrenadeSweepContext *context)
{
    *context->owner_flags = context->saved_flags;
    restore_effect_registers(&context->registers);
}

static int checked_grenade_origin(const TsfpGrenadeSource *source, uint32_t kind,
                                  XrVector3f *origin, uint32_t *room)
{
    GrenadeSweepContext context;
    if (!begin_grenade_sweeps(&context, source, kind))
        return 0;
    const int ready = tsfp_grenade_clear_origin(source, tsfp_grenade_probe_drop(kind),
                                                grenade_sweep, &context, origin, room);
    end_grenade_sweeps(&context);
    return ready;
}

typedef struct {
    uint32_t args[16];
    XrVector3f position, directions[3];
} GrenadeConstructorScratch;

static int redirect_grenade_constructor(uint32_t stack)
{
    enum { KIND = 1, ORIGIN, RAY_ORIGIN, MODEL_ORIGIN, RAY, SHOOTER = 9, ROOM = 13 };
    if (stack < 0x2000u)
        return 0;
    const uint32_t kind = MEM32(stack + KIND * 4u);
    if (!tsfp_grenade_weapon_for_kind(kind))
        return 0;

    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t base = (stack - sizeof(GrenadeConstructorScratch)) & ~15u;
    GrenadeConstructorScratch *guest = vm.resolve(vm.host, base, sizeof *guest, 1);
    const uint32_t *original = vm.resolve(vm.host, stack, sizeof guest->args, 0);
    if (!original || !guest || !original[SHOOTER])
        return 0;

    XrVector3f directions[3];
    for (unsigned i = 0; i < 3; ++i) {
        const void *vector = vm.resolve(vm.host, original[RAY + i], sizeof directions[i], 0);
        if (!vector)
            return 0;
        memcpy(&directions[i], vector, sizeof directions[i]);
        if (!tsfp_grenade_vector_valid(directions[i]))
            return 0;
    }
    XrVector3f position;
    TsfpGrenadeSource source;
    if (!tsfp_stereo_grenade_launch(original[SHOOTER], &source, &position, directions))
        return 0;
    for (unsigned i = 0; i < 3; ++i)
        if (!tsfp_grenade_vector_valid(directions[i]))
            return 0;

    uint32_t room;
    const int has_origin = checked_grenade_origin(&source, kind, &position, &room);
    /* The collision call also uses the guest stack. Stage the constructor
     * arguments after it returns. EAAF0 copies the input vectors. */
    memcpy(guest->args, original, sizeof guest->args);
    if (has_origin) {
        guest->position = position;
        const uint32_t address = base + offsetof(GrenadeConstructorScratch, position);
        guest->args[ORIGIN] = guest->args[RAY_ORIGIN] = guest->args[MODEL_ORIGIN] = address;
        guest->args[ROOM] = room;
    }
    memcpy(guest->directions, directions, sizeof directions);
    for (unsigned i = 0; i < 3; ++i)
        guest->args[RAY + i] = base + offsetof(GrenadeConstructorScratch, directions) + i * sizeof directions[i];
    g_esp = base;
    return 1;
}

void tsfp_quest_grenade_constructor(void)
{
    const uint32_t stack = g_esp;
    const int redirected = redirect_grenade_constructor(stack);
    sub_000EAAF0();
    if (redirected)
        g_esp = stack + 4u;
}

static void draw_grenade_preview(void)
{
    TsfpGrenadePreview preview;
    GrenadeSweepContext context;
    if (!tsfp_vk_uplink_world_bound() || !tsfp_stereo_grenade_preview(&preview) ||
        !begin_grenade_sweeps(&context, &preview.source, preview.flight.kind))
        return;

    TsfpGrenadeArc arc = {.count = 0};
    uint32_t room;
    if (tsfp_grenade_clear_origin(&preview.source, preview.flight.probe_drop, grenade_sweep,
                                  &context, &preview.flight.origin, &room))
        tsfp_grenade_trace(&preview.flight, room, grenade_sweep, &context, &arc);
    end_grenade_sweeps(&context);

    if (arc.count > 1) {
        flush_guest_draws();
        tsfp_grenade_render(&arc, &preview.view);
    }
}

void tsfp_quest_first_person_weapon_pass(void)
{
    const int immersive = tsfp_stereo_immersive_player();
    RhiView *scope = NULL;
    if (tsfp_vk_scope_recording()) {
        flush_guest_draws();
        scope = tsfp_vk_scope_finish();
    }
    if (immersive) {
        tsfp_vk_shield_capture(1);
        tsfp_stereo_first_person_weapon_pass(1);
    }
    sub_001B9120();
    if (immersive) {
        tsfp_stereo_first_person_weapon_pass(0);
        if (scope) {
            flush_guest_draws();
            tsfp_scope_render(scope);
        }
        tsfp_vk_shield_capture(2);
        draw_grenade_preview();
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
    if (!tsfp_stereo_prepare_uplink_map(g_edi, MEM32(g_esp + 8u), MEM32(g_esp + 12u))) {
        sub_0034CAA0();
        return;
    }
    flush_guest_draws();
    const TsfpModelRole previous = tsfp_stereo_set_model_role(TSFP_MODEL_UPLINK_MAP);
    sub_0034CAA0();
    flush_guest_draws();
    tsfp_stereo_set_model_role(previous);
}

void tsfp_quest_uplink_map_source(void)
{
    /* 34DD20 registers the handheld map in radar slot 0 when source type is 1. */
    if (!tsfp_vk_stereo_geometry() || !MEM32(0x007676B8u) || MEM32(0x007676DCu) != 1u) {
        sub_0034B700();
        return;
    }
    flush_guest_draws();
    tsfp_stereo_set_uplink_map_source(1);
    sub_0034B700();
    flush_guest_draws();
    tsfp_stereo_set_uplink_map_source(0);
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
