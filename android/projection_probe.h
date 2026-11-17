#ifndef TSFP_PROJECTION_PROBE_H
#define TSFP_PROJECTION_PROBE_H

#include <openxr/openxr.h>
#include <vulkan/vulkan.h>
#include "stereo_probe.h"

int tsfp_projection_create(XrSession session, VkDevice device, VkFormat format,
                           uint32_t geometry_width, uint32_t geometry_height);
void tsfp_projection_begin(XrSpace local, const XrPosef *anchor,
                           const XrView eyes[TSFP_XR_EYES]);
void tsfp_projection_draw(VkCommandBuffer commands);
VkImage tsfp_projection_geometry_image(void);
void tsfp_projection_geometry_submitted(void);
int tsfp_projection_has_geometry(void);
void tsfp_projection_forget_geometry(void);
const XrCompositionLayerProjection *tsfp_projection_end(int reuse_geometry);
void tsfp_projection_destroy(void);

#endif
