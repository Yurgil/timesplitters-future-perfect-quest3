#ifndef TSFP_UPLINK_MATH_H
#define TSFP_UPLINK_MATH_H

#include "weapon_mount.h"

enum { TSFP_UPLINK = 36, TSFP_UPLINK_MAP = 37 };
static const float tsfp_uplink_range = 50.0f;

typedef struct {
    XrVector3f origin, forward, up;
} TsfpUplinkRay;

static inline void tsfp_uplink_rotate_map(const XrMatrix4x4f *cuff_move, XrVector3f pivot,
                                          XrVector3f elbow, XrVector3f wrist, XrMatrix4x4f *map_move)
{
    XrVector3f axis;
    XrVector3f_Sub(&axis, &wrist, &elbow);
    XrVector3f_Normalize(&axis);
    /* The axis points away from the observer looking toward the wrist. */
    const float sin_45 = .70710678f;
    const XrQuaternionf ccw = {-axis.x * sin_45, -axis.y * sin_45, -axis.z * sin_45, sin_45};
    XrMatrix4x4f rotation;
    XrMatrix4x4f_CreateFromQuaternion(&rotation, &ccw);
    XrVector3f rotated_pivot, offset;
    XrMatrix4x4f_TransformVector3f(&rotated_pivot, &rotation, &pivot);
    XrVector3f_Sub(&offset, &pivot, &rotated_pivot);
    tsfp_matrix_set_position(&rotation, offset);
    XrMatrix4x4f_Multiply(map_move, &rotation, cuff_move);
}

static inline XrVector3f tsfp_uplink_ray_delta(const TsfpUplinkRay *ray, float distance)
{
    XrVector3f point;
    XrVector3f_Scale(&point, &ray->forward, distance);
    return point;
}

static inline XrVector3f tsfp_uplink_point(const TsfpUplinkRay *ray, float distance)
{
    XrVector3f point = tsfp_uplink_ray_delta(ray, distance);
    XrVector3f_Add(&point, &point, &ray->origin);
    return point;
}

static inline int tsfp_uplink_view(const TsfpUplinkRay *ray, const XrMatrix4x4f *lens, XrMatrix4x4f *clip)
{
    XrVector3f back;
    XrVector3f_Scale(&back, &ray->forward, -1.0f);
    XrMatrix4x4f from_aim, view;
    if (!tsfp_weapon_basis(&from_aim, back, ray->up))
        return 0;
    tsfp_matrix_set_position(&from_aim, ray->origin);
    XrMatrix4x4f_InvertRigidBody(&view, &from_aim);
    XrMatrix4x4f_Multiply(clip, lens, &view);
    return 1;
}

static inline int tsfp_uplink_entity_address(uint32_t table, uint32_t handle, uint32_t *address)
{
    const uint64_t result = (uint64_t)table + (handle >> 22) * 0x2e8u;
    if (!table || !handle || result > UINT32_MAX)
        return 0;
    *address = (uint32_t)result;
    return 1;
}

#endif
