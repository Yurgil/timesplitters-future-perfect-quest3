#ifndef TSFP_STEREO_BACKEND_H
#define TSFP_STEREO_BACKEND_H

#include "rhi.h"

#define TSFP_UPLINK_UBO_SLOT 6

int tsfp_vk_stereo_geometry(void);
void tsfp_vk_stereo_size(uint32_t *width, uint32_t *height);
int tsfp_vk_stereo_allocating(int active);
void tsfp_vk_stereo_world(RhiImage *color);
void tsfp_vk_stereo_frame_copy(RhiImage *image, int full_frame);
int tsfp_vk_stereo_snapshot(RhiImage *destination);
void tsfp_vk_stereo_draw(int world);
void tsfp_vk_avatar_capture_begin(void);
void tsfp_vk_avatar_capture_end(void);
int tsfp_vk_uplink_world_bound(void);
RhiImage *tsfp_vk_uplink_source(void);
void tsfp_vk_uplink_eye(unsigned mask);
void tsfp_vk_shield_capture(unsigned stage);
void tsfp_uplink_render_destroy(void);
const RhiMacro *tsfp_vk_stereo_macros(void);

#endif
