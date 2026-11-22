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

static inline void tsfp_uplink_face_map(XrMatrix4x4f *map_move, const XrMatrix4x4f *plane,
                                        XrVector3f apex, XrVector3f eyes)
{
    XrMatrix4x4f map;
    XrMatrix4x4f_Multiply(&map, map_move, plane);
    const XrVector3f right = {map.m[0], map.m[1], map.m[2]};
    const XrVector3f up = {map.m[4], map.m[5], map.m[6]};
    XrVector3f normal;
    XrVector3f_Cross(&normal, &right, &up);
    if (!(XrVector3f_Length(&normal) > 1e-5f))
        return;
    XrVector3f_Normalize(&normal);

    XrVector3f pivot, center, to_eyes, to_center;
    /* Guest map corners span X [-.09, .09], Y [.175, .335], and Z = 0. */
    XrMatrix4x4f_TransformVector3f(&center, &map, &(XrVector3f){0, .255f, 0});
    XrMatrix4x4f_TransformVector3f(&pivot, map_move, &apex);
    XrVector3f_Sub(&to_eyes, &eyes, &pivot);
    XrVector3f_Sub(&to_center, &center, &pivot);

    /* Find where the eyes would be relative to the tip if the current
     * plane faced them. Rotate that ray to the actual eyes around the tip. */
    XrVector3f normal_offset, tangent;
    XrVector3f_Scale(&normal_offset, &normal, XrVector3f_Dot(&to_center, &normal));
    XrVector3f_Sub(&tangent, &to_center, &normal_offset);
    const float forward_sq = XrVector3f_Dot(&to_eyes, &to_eyes) - XrVector3f_Dot(&tangent, &tangent);
    if (!(forward_sq > 1e-5f))
        return;
    const float forward = sqrtf(forward_sq);
    if (!(forward > XrVector3f_Dot(&to_center, &normal)))
        return;
    XrVector3f_Scale(&normal_offset, &normal, forward);
    XrVector3f from, axis;
    XrVector3f_Add(&from, &tangent, &normal_offset);
    XrVector3f_Normalize(&from);
    XrVector3f_Normalize(&to_eyes);
    XrVector3f_Cross(&axis, &from, &to_eyes);
    const float dot = XrVector3f_Dot(&from, &to_eyes);
    XrQuaternionf turn = {axis.x, axis.y, axis.z, 1.0f + dot};
    if (dot < -.999999f) {
        XrMatrix4x4f basis;
        if (!tsfp_weapon_basis(&basis, from, up) && !tsfp_weapon_basis(&basis, from, right))
            return;
        turn = (XrQuaternionf){basis.m[4], basis.m[5], basis.m[6], 0};
    }
    XrQuaternionf_Normalize(&turn);
    XrMatrix4x4f rotation, faced;
    XrMatrix4x4f_CreateFromQuaternion(&rotation, &turn);
    XrVector3f rotated_pivot, offset;
    XrMatrix4x4f_TransformVector3f(&rotated_pivot, &rotation, &pivot);
    XrVector3f_Sub(&offset, &pivot, &rotated_pivot);
    tsfp_matrix_set_position(&rotation, offset);
    XrMatrix4x4f_Multiply(&faced, &rotation, map_move);
    *map_move = faced;
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
