#ifndef TSFP_STEREO_BACKEND_H
#define TSFP_STEREO_BACKEND_H

#include "rhi.h"

#define TSFP_UPLINK_UBO_SLOT 6
enum { TSFP_MAG_CHARGER_VISION_ROUTINE = 0x00344EA0 };

int tsfp_vk_stereo_geometry(void);
void tsfp_vk_stereo_size(uint32_t *width, uint32_t *height);
int tsfp_vk_stereo_allocating(int active);
void tsfp_vk_stereo_world(RhiImage *color);
void tsfp_vk_stereo_frame_copy(RhiImage *image, int full_frame);
int tsfp_vk_stereo_snapshot_extent(uint32_t *width, uint32_t *height);
int tsfp_vk_stereo_effect_extent(uint32_t *width, uint32_t *height);
int tsfp_vk_mag_charger_extent(uint32_t *width, uint32_t *height);
int tsfp_vk_stereo_snapshot(RhiImage *destination);
int tsfp_vk_cinematic_matte(uint32_t caller, int top, float edge);
void tsfp_vk_stereo_draw(int world);
void tsfp_vk_avatar_capture_begin(void);
void tsfp_vk_avatar_capture_end(void);
int tsfp_vk_uplink_world_bound(void);
int tsfp_vk_ghost_ready(void);
void tsfp_vk_ghost_eye(unsigned eye);
RhiImage *tsfp_vk_uplink_source(void);
void tsfp_vk_uplink_eye(unsigned mask);
void tsfp_vk_shield_capture(unsigned stage);
int tsfp_vk_scope_supported(void);
int tsfp_vk_scope_recording(void);
RhiView *tsfp_vk_scope_finish(void);
void tsfp_scope_render(RhiView *image);
void tsfp_scope_render_destroy(void);
void tsfp_uplink_render_destroy(void);
const RhiMacro *tsfp_vk_stereo_macros(void);

#endif
