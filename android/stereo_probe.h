#ifndef TSFP_STEREO_PROBE_H
#define TSFP_STEREO_PROBE_H

#include <openxr/openxr.h>
#include "avatar_model.h"

enum { TSFP_XR_EYES = 2 };
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
unsigned tsfp_xr_avatar_poses(XrPosef *anchor, XrVector3f *body_offset,
                             XrPosef hands[TSFP_AVATAR_HANDS]);
int tsfp_xr_body_matrices(XrVector3f body_offset, float rows[32]);
int tsfp_stereo_body_matrices(float rows[32]);
int tsfp_stereo_held_matrices(unsigned hand, float rows[32]);
void tsfp_stereo_overlay(int begin);
int tsfp_stereo_hud_draw(void);
int tsfp_stereo_screen_effect_draw(void);
int tsfp_stereo_legacy_weapon_draw(void);
int tsfp_xr_hud_matrices(float rows[32]);
int tsfp_xr_held_matrices(XrVector3f body_offset, const float camera_correction[16], float rows[32]);
struct XrMatrix4x4f;
int tsfp_stereo_weapon_attachment(TsfpModelRole role, XrVector3f *body_offset, struct XrMatrix4x4f *camera_move);
int tsfp_stereo_weapon_effect_attachment(TsfpModelRole role, XrVector3f *body_offset,
                                         struct XrMatrix4x4f *camera_move);
TsfpModelRole tsfp_stereo_weapon_particles(uint32_t emitter);
int tsfp_stereo_prepare_uplink_map(uint32_t state);
TsfpModelRole tsfp_stereo_set_model_role(TsfpModelRole role);
int tsfp_stereo_weapon_pose(TsfpModelRole role, XrPosef *pose);
void tsfp_stereo_apply_snap_turn(void);
int tsfp_stereo_mounted_gun(void);
int tsfp_stereo_sniper_shield_active(void);
int tsfp_stereo_uplink_physics_begin(uint32_t target);
void tsfp_stereo_uplink_physics_end(void);
int tsfp_xr_take_snap_turn(void);
int tsfp_xr_take_hand_calibration(void);
const char *tsfp_xr_hand_calibration_file(void);
void tsfp_stereo_probe_frame(XrSession session, XrSpace local, XrTime display_time,
                            const XrPosef *anchor, const XrView *eyes, uint32_t frame);

#endif
