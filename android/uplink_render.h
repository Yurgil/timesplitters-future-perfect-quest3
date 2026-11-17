#ifndef TSFP_UPLINK_RENDER_H
#define TSFP_UPLINK_RENDER_H

#include <openxr/openxr.h>

typedef struct {
    XrVector3f start, end, marker;
    float age, fade;
    int target, held, beam;
} TsfpUplinkVisual;

unsigned tsfp_uplink_project_beam(const TsfpUplinkVisual *visual, float width, float height,
                                  XrVector4f segments[2]);
void tsfp_uplink_render_marker(const TsfpUplinkVisual *visual);
void tsfp_uplink_render_destroy(void);

#endif
