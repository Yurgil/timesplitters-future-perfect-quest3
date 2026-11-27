#ifndef TSFP_STEREO_PROBE_H
#define TSFP_STEREO_PROBE_H

#include <openxr/openxr.h>
#include "avatar_model.h"

enum { TSFP_XR_EYES = 2 };
enum { TSFP_GHOST_CAMERA_VIEW, TSFP_GHOST_CAMERA_PROJECTION, TSFP_GHOST_CAMERA_VIEW_PROJECTION, TSFP_GHOST_CAMERA_MATRICES };
typedef struct {
    uint32_t address[TSFP_GHOST_CAMERA_MATRICES];
    void *matrix[TSFP_GHOST_CAMERA_MATRICES];
    float original[TSFP_GHOST_CAMERA_MATRICES][16];
    float eyes[TSFP_XR_EYES][TSFP_GHOST_CAMERA_MATRICES][16];
} TsfpGhostCamera;
typedef enum {
    TSFP_MODEL_DEFAULT,
    TSFP_MODEL_LEFT_WEAPON,
    TSFP_MODEL_RIGHT_WEAPON,
    TSFP_MODEL_LEFT_FLASH,
    TSFP_MODEL_RIGHT_FLASH,
    TSFP_MODEL_LEFT_SHIELD,
    TSFP_MODEL_RIGHT_SHIELD,
    TSFP_MODEL_LEFT_WEAPON_PARTICLES,
    TSFP_MODEL_RIGHT_WEAPON_PARTICLES,
    TSFP_MODEL_MUZZLE_PARTICLES,
    TSFP_MODEL_UPLINK_MAP,
    TSFP_MODEL_TRACKED_COUNT,
    TSFP_MODEL_BODY,
    TSFP_MODEL_LEGACY_ARMS,
    TSFP_MODEL_HIDDEN,
    TSFP_MODEL_LEFT_HELD,
    TSFP_MODEL_RIGHT_HELD
} TsfpModelRole;

/* The body and the world weapons in its hands share the world depth range. */
static inline int tsfp_model_role_on_body(TsfpModelRole role)
{
    return role == TSFP_MODEL_BODY || role == TSFP_MODEL_LEFT_HELD || role == TSFP_MODEL_RIGHT_HELD;
}

int tsfp_xr_locate_stereo(XrSession session, XrSpace local, XrTime display_time,
                          XrView eyes[TSFP_XR_EYES]);
int tsfp_stereo_world_projection(float projection[16]);
int tsfp_stereo_world_scene(void);
int tsfp_stereo_ghost_camera(TsfpGhostCamera *camera);
const float *tsfp_stereo_weapon_projection(void);
void tsfp_stereo_probe_enable(int enabled);
void tsfp_stereo_probe_camera(uint32_t caller);
void tsfp_stereo_probe_weapon(uint32_t address);
void tsfp_stereo_begin_frame(void);
int tsfp_stereo_avatar_after_cull(uint32_t entity);
void tsfp_stereo_note_body_draw(void);
int tsfp_stereo_model_begin(uint32_t instance, uint32_t node, uint32_t view);
void tsfp_stereo_model_end(int previous);
TsfpModelRole tsfp_stereo_model_role(void);
int tsfp_stereo_weapon_depth_range(float *offset, float *scale);
int tsfp_xr_tracked_matrices(const float projection[16], TsfpModelRole role, float rows[32]);
int tsfp_xr_avatar_ready(void);
int tsfp_xr_hand_pose(unsigned hand, XrPosef *anchor, XrPosef *pose);
unsigned tsfp_xr_avatar_poses(XrPosef *anchor, XrVector3f *body_offset,
                             XrPosef hands[TSFP_AVATAR_HANDS]);
int tsfp_xr_body_matrices(XrVector3f body_offset, float rows[32]);
int tsfp_stereo_body_matrices(float rows[32]);
int tsfp_stereo_held_matrices(unsigned hand, float rows[32]);
void tsfp_stereo_overlay(int begin);
int tsfp_stereo_hud_draw(void);
typedef enum {
    TSFP_RETICLE_NONE,
    TSFP_RETICLE_TURRET,
    TSFP_RETICLE_MECH,
} TsfpReticleDraw;
void tsfp_stereo_turret_reticle(TsfpReticleDraw draw);
int tsfp_stereo_turret_reticle_draw(void);
int tsfp_stereo_mech_reticle_center(XrVector2f *center);
int tsfp_xr_turret_reticle_matrices(float rows[32]);
int tsfp_stereo_screen_effect_draw(void);
typedef enum {
    TSFP_SCREEN_OVERLAY_NONE,
    TSFP_SCREEN_OVERLAY_STEREO,
    TSFP_SCREEN_OVERLAY_SCENE,
} TsfpScreenOverlay;
TsfpScreenOverlay tsfp_stereo_set_screen_overlay(TsfpScreenOverlay draw);
TsfpScreenOverlay tsfp_stereo_screen_overlay_draw(void);
int tsfp_stereo_legacy_weapon_draw(void);
int tsfp_xr_hud_matrices(float rows[32]);
int tsfp_xr_held_matrices(XrVector3f body_offset, const float camera_correction[16], float rows[32]);
struct XrMatrix4x4f;
struct TsfpScopeGeometry;
uint32_t tsfp_stereo_scope_weapon(unsigned *hand);
int tsfp_stereo_scope_camera(uint32_t id, struct TsfpScopeGeometry *scope);
int tsfp_stereo_scope_lens(uint32_t id, struct TsfpScopeGeometry *scope, XrVector3f *body_offset);
int tsfp_stereo_weapon_attachment(TsfpModelRole role, XrVector3f *body_offset, struct XrMatrix4x4f *camera_move);
int tsfp_stereo_weapon_effect_attachment(TsfpModelRole role, XrVector3f *body_offset,
                                         struct XrMatrix4x4f *camera_move);
TsfpModelRole tsfp_stereo_weapon_particles(uint32_t emitter);
int tsfp_stereo_prepare_uplink_map(uint32_t state, uint32_t plane, uint32_t apex);
void tsfp_stereo_set_uplink_map_source(int draw);
int tsfp_stereo_uplink_map_source(void);
XrVector3f tsfp_xr_body_eye_position(XrVector3f body_offset);
TsfpModelRole tsfp_stereo_set_model_role(TsfpModelRole role);
int tsfp_stereo_weapon_pose(TsfpModelRole role, XrPosef *pose);
void tsfp_stereo_apply_snap_turn(void);

static inline int tsfp_turret_uses_aim_bindings(uint32_t mode)
{
    /* 25D520 sends these modes to the mounted and stationary aim updates. */
    return mode == 0x40u || mode == 0x80u;
}

static inline int tsfp_turret_camera_mode(uint32_t mode)
{
    /* B4D04 starts mounting (0x20), and B6989 switches to 0x40.
     * 0x10000 uses the separate dual-turret controls. */
    return mode == 0x20u || tsfp_turret_uses_aim_bindings(mode) || mode == 0x10000u;
}

enum {
    TSFP_CAMERA_SCRIPTED = 0x100u,
    TSFP_CAMERA_BATTLE_MECH = 0x8000u,
};

typedef enum {
    TSFP_VEHICLE_LOOK_DEFAULT,
    TSFP_VEHICLE_LOOK_GUNNER,
    TSFP_VEHICLE_LOOK_TRACKED_DRIVER,
    TSFP_VEHICLE_LOOK_MECH,
} TsfpVehicleLook;

int tsfp_stereo_in_vehicle(void);
TsfpVehicleLook tsfp_stereo_vehicle_look(void);
int tsfp_stereo_mounted_gun(void);
int tsfp_stereo_cart_riding(void);
void tsfp_stereo_cart_update(void (*update)(void));
int tsfp_stereo_cart_visible(uint32_t entity, uint32_t view);
int tsfp_stereo_sniper_shield_active(void);
int tsfp_stereo_uplink_physics_begin(uint32_t target);
void tsfp_stereo_uplink_physics_end(void);
int tsfp_xr_take_snap_turn(void);
int tsfp_xr_take_hand_calibration(void);
const char *tsfp_xr_hand_calibration_file(void);
const char *tsfp_xr_legacy_hand_calibration_file(void);
void tsfp_stereo_probe_frame(XrSession session, XrSpace local, XrTime display_time,
                            const XrPosef *anchor, const XrView *eyes, uint32_t frame);

#endif
