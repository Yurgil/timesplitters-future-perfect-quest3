#ifndef TSFP_STEREO_MATH_H
#define TSFP_STEREO_MATH_H

#include "xr_linear.h"

/* The stick's forward axis is positive. Project the head's forward vector
 * onto the game's yaw plane so that looking down does not slow walking. */
static inline XrVector2f tsfp_head_relative_stick(XrVector2f stick, XrQuaternionf head,
                                               XrQuaternionf anchor)
{
    const XrVector3f forward = { 0, 0, -1 };
    XrQuaternionf local_to_anchor;
    XrVector3f direction, relative;
    XrQuaternionf_Invert(&local_to_anchor, &anchor);
    XrQuaternionf_RotateVector3f(&direction, &head, &forward);
    XrQuaternionf_RotateVector3f(&relative, &local_to_anchor, &direction);
    const float horizontal = hypotf(relative.x, relative.z);
    if (horizontal < 0.1f)
        return stick;
    const float cosine = -relative.z / horizontal, sine = -relative.x / horizontal;
    return (XrVector2f){ cosine * stick.x - sine * stick.y, sine * stick.x + cosine * stick.y };
}

static inline int tsfp_stick_step(float axis, int *armed)
{
    const float distance = fabsf(axis);
    if (distance < 0.35f) {
        *armed = 1;
        return 0;
    }
    if (!*armed || distance < 0.70f)
        return 0;
    *armed = 0;
    return axis > 0 ? 1 : -1;
}

typedef struct {
    int snap_armed, weapon_armed, weapon_step, weapon_frames;
} TsfpRightStickState;

typedef struct {
    XrVector2f look;
    int snap_degrees, weapon;
} TsfpRightStickInput;

static inline TsfpRightStickInput tsfp_right_stick(XrVector2f stick, int mounted, int snap_degrees,
                                                  TsfpRightStickState *state)
{
    if (mounted) {
        *state = (TsfpRightStickState){0};
        return (TsfpRightStickInput){ .look = stick };
    }

    TsfpRightStickInput input = {0};
    const float horizontal = fabsf(stick.x), vertical = fabsf(stick.y);
    if (vertical < 0.35f || vertical > horizontal) {
        const int step = tsfp_stick_step(stick.y, &state->weapon_armed);
        if (step) {
            state->weapon_step = step;
            state->weapon_frames = 2; /* Keep a short flick visible to the guest input poll. */
        }
    }
    if (state->weapon_frames) {
        input.weapon = state->weapon_step;
        --state->weapon_frames;
    }

    if (snap_degrees) {
        if (horizontal < 0.35f || horizontal >= vertical)
            input.snap_degrees = snap_degrees * tsfp_stick_step(stick.x, &state->snap_armed);
    } else if (horizontal >= vertical || vertical < 0.65f) {
        input.look.x = stick.x;
    }
    return input;
}

/* A passenger's view can turn relative to the cart without feeding the
 * added yaw back into the guest's automatic seat-facing update. */
static inline float tsfp_cart_turn(float offset, float stick, int snap, float seconds)
{
    const float smooth = 90.0f * stick * fminf(0.1f, fmaxf(0.0f, seconds));
    return remainderf(offset - (snap ? snap : smooth), 360.0f);
}

/* Rotate the source mesh around its wrist and keep the wrist exactly at
 * the tracked grip, independent of the current head pose. */
static inline XrPosef tsfp_wrist_pose(XrVector3f grip, XrQuaternionf model_orientation,
                                    XrVector3f wrist)
{
    XrVector3f offset;
    XrQuaternionf_RotateVector3f(&offset, &model_orientation, &wrist);
    return (XrPosef){ .orientation = model_orientation,
        .position = { grip.x - offset.x, grip.y - offset.y, grip.z - offset.z } };
}

/* Touch grip is centered on the palm. Its axes differ from the aim ray.
 * Use the fixed grip-to-aim rotation measured on the Touch Plus profile
 * when the runtime has a grip pose but has lost the aim pose. */
static inline XrQuaternionf tsfp_touch_aim_from_grip(XrQuaternionf grip, unsigned hand)
{
    const float side = hand == 0 ? -1.0f : 1.0f;
    XrQuaternionf grip_from_aim = { 0.037f, side * 0.500f, side * 0.865f, 0.021f };
    XrQuaternionf_Normalize(&grip_from_aim);
    XrQuaternionf aim;
    XrQuaternionf_Multiply(&aim, &grip_from_aim, &grip);
    return aim;
}

/* The head turns about the neck, below and behind the eyes. Following this
 * point keeps the body from sliding forward when the player looks down. */
static inline XrVector3f tsfp_head_pivot(const XrPosef *head)
{
    const XrVector3f neck = { 0.0f, -0.075f, 0.08f };
    XrVector3f pivot;
    XrQuaternionf_RotateVector3f(&pivot, &head->orientation, &neck);
    XrVector3f_Add(&pivot, &pivot, &head->position);
    return pivot;
}

/* Move the rendered body with the player's horizontal room-scale motion.
 * Keep it close to the guest collision capsule and keep its feet on the
 * guest floor when the player crouches or stands up. */

static inline XrVector3f tsfp_avatar_room_offset(XrVector3f anchor, XrVector3f head)
{
    XrVector3f offset = { head.x - anchor.x, 0.0f, head.z - anchor.z };
    const float distance = hypotf(offset.x, offset.z);
    if (distance > 0.35f)
        XrVector3f_Scale(&offset, &offset, 0.35f / distance);
    return offset;
}

static inline XrMatrix4x4f tsfp_eye_from_anchor(const XrPosef *anchor, const XrView *eye)
{
    XrMatrix4x4f local_from_anchor, local_from_eye, eye_from_local;
    XrMatrix4x4f eye_from_anchor;
    XrMatrix4x4f_CreateFromRigidTransform(&local_from_anchor, anchor);
    XrMatrix4x4f_CreateFromRigidTransform(&local_from_eye, &eye->pose);
    XrMatrix4x4f_InvertRigidBody(&eye_from_local, &local_from_eye);
    XrMatrix4x4f_Multiply(&eye_from_anchor, &eye_from_local, &local_from_anchor);
    return eye_from_anchor;
}

/* Anchor and eye poses share the OpenXR local space. The output maps the
 * yaw-recentered anchor's meter coordinates to this eye's clip space. */
static inline XrMatrix4x4f tsfp_eye_clip_for_api(const XrPosef *anchor, const XrView *eye,
                                               float near_z, float far_z, GraphicsAPI api)
{
    const XrMatrix4x4f eye_from_anchor = tsfp_eye_from_anchor(anchor, eye);
    XrMatrix4x4f clip_from_eye, clip_from_anchor;
    XrMatrix4x4f_CreateProjectionFov(&clip_from_eye, api, eye->fov, near_z, far_z);
    XrMatrix4x4f_Multiply(&clip_from_anchor, &clip_from_eye, &eye_from_anchor);
    return clip_from_anchor;
}

static inline XrMatrix4x4f tsfp_eye_clip(const XrPosef *anchor, const XrView *eye,
                                       float near_z, float far_z)
{
    return tsfp_eye_clip_for_api(anchor, eye, near_z, far_z, GRAPHICS_VULKAN);
}

/* Enclose both eye frusta with the union of their viewing directions.
 * Move the apex back to contain the displaced eye origins as well. */
static inline int tsfp_stereo_visibility_clip(XrMatrix4x4f *out, const XrPosef *anchor,
                                               const XrPosef *head, const XrView *views, unsigned count,
                                               float near_z, float far_z)
{
    XrPosef head_from_local;
    XrPosef_Invert(&head_from_local, head);
    float left = INFINITY, right = -INFINITY, down = INFINITY, up = -INFINITY;
    XrVector3f origins[3];
    if (count < 2 || count > 3)
        return 0;
    for (unsigned eye = 0; eye < count; ++eye) {
        XrPosef relative;
        XrPosef_Multiply(&relative, &head_from_local, &views[eye].pose);
        origins[eye] = relative.position;
        const XrFovf fov = views[eye].fov;
        const float x[2] = { tanf(fov.angleLeft), tanf(fov.angleRight) };
        const float y[2] = { tanf(fov.angleDown), tanf(fov.angleUp) };
        for (unsigned i = 0; i < 2; ++i) {
            for (unsigned j = 0; j < 2; ++j) {
                const XrVector3f corner = { x[i], y[j], -1.0f };
                XrVector3f ray;
                XrQuaternionf_RotateVector3f(&ray, &relative.orientation, &corner);
                if (!(ray.z < 0.0f))
                    return 0;
                left = fminf(left, -ray.x / ray.z);
                right = fmaxf(right, -ray.x / ray.z);
                down = fminf(down, -ray.y / ray.z);
                up = fmaxf(up, -ray.y / ray.z);
            }
        }
    }
    if (!(left < 0.0f && right > 0.0f && down < 0.0f && up > 0.0f))
        return 0;
    float setback = 0.0f;
    for (unsigned eye = 0; eye < count; ++eye) {
        const XrVector3f p = origins[eye];
        const float horizontal = fmaxf(p.x / left, p.x / right);
        const float vertical = fmaxf(p.y / down, p.y / up);
        setback = fmaxf(setback, p.z + fmaxf(horizontal, vertical));
    }
    if (!isfinite(setback))
        return 0;
    XrView visibility_eye = { .pose = *head,
        .fov = { .angleLeft = atanf(left), .angleRight = atanf(right),
                 .angleDown = atanf(down), .angleUp = atanf(up) } };
    const XrVector3f offset = { .z = setback };
    XrPosef_TransformVector3f(&visibility_eye.pose.position, head, &offset);
    *out = tsfp_eye_clip_for_api(anchor, &visibility_eye, near_z, far_z + setback, GRAPHICS_D3D);
    return 1;
}

enum { TSFP_FRUSTUM_PLANES = 6 };

static inline void tsfp_clip_planes(XrVector4f planes[TSFP_FRUSTUM_PLANES], const XrMatrix4x4f *clip)
{
    const float *m = clip->m;
    const unsigned axes[TSFP_FRUSTUM_PLANES] = { 0, 0, 1, 1, 2, 2 };
    for (unsigned i = 0; i < TSFP_FRUSTUM_PLANES; ++i) {
        const unsigned row = axes[i];
        const float sign = (i & 1u) ? -1.0f : 1.0f;
        const float w = i == 4u ? 0.0f : 1.0f;
        planes[i] = (XrVector4f){ w * m[3] + sign * m[row], w * m[7] + sign * m[4 + row],
                                 w * m[11] + sign * m[8 + row], w * m[15] + sign * m[12 + row] };
    }
}

static inline int tsfp_box_outside_frustum(const XrVector4f planes[TSFP_FRUSTUM_PLANES],
                                           XrVector3f minimum, XrVector3f maximum)
{
    for (unsigned i = 0; i < TSFP_FRUSTUM_PLANES; ++i) {
        const XrVector4f p = planes[i];
        const float x = p.x >= 0.0f ? maximum.x : minimum.x;
        const float y = p.y >= 0.0f ? maximum.y : minimum.y;
        const float z = p.z >= 0.0f ? maximum.z : minimum.z;
        if (p.x * x + p.y * y + p.z * z + p.w < -0.001f)
            return 1;
    }
    return 0;
}

/* Perspective depth is 0 at near and 1 at far. The US Xbox camera uses
 * -Z forward (p[11] == -1); the sign also supports D3D's +Z convention. */
static inline int tsfp_xbox_camera_planes(const XrMatrix4x4f *projection, float *near_z, float *far_z)
{
    const float *p = projection->m;
    for (unsigned i = 0; i < 16; ++i)
        if (!isfinite(p[i]))
            return 0;
    const float clip_w = fabsf(p[11]);
    const float depth_scale = p[11] < 0 ? -p[10] : p[10];
    if (!(p[0] > 0 && p[5] > 0 && clip_w > 0 && depth_scale > clip_w && p[14] < 0) ||
        p[1] || p[2] || p[3] || p[4] || p[6] || p[7] || p[12] || p[13] || p[15])
        return 0;
    *near_z = -p[14] / depth_scale;
    *far_z = -p[14] / (depth_scale - clip_w);
    return isfinite(*far_z) && *near_z > 0 && *far_z > *near_z;
}

/* NV2A vertex programs bake the viewport's depth range into clip Z. */
static inline void tsfp_xbox_pack_depth(XrMatrix4x4f *projection, float offset, float scale)
{
    for (unsigned column = 0; column < 4; ++column) {
        float *z = &projection->m[column * 4 + 2];
        *z = scale * *z + offset * projection->m[column * 4 + 3];
    }
}

/* Guest effects that project actor bounds on the CPU need the actual
 * per-eye view and projection, as well as their product. */
static inline void tsfp_xbox_eye_camera(const XrMatrix4x4f *game_view, const XrPosef *anchor,
                                       const XrView *eye, float near_z, float far_z,
                                       XrMatrix4x4f *view, XrMatrix4x4f *projection,
                                       XrMatrix4x4f *view_projection)
{
    const XrMatrix4x4f eye_from_game = tsfp_eye_from_anchor(anchor, eye);
    XrMatrix4x4f_Multiply(view, &eye_from_game, game_view);
    XrMatrix4x4f_CreateProjectionFov(projection, GRAPHICS_D3D, eye->fov, near_z, far_z);
    XrMatrix4x4f_Multiply(view_projection, projection, view);
}

/* Changes a game's clip-space vertex before rasterization. The game lens
 * provides its depth range, and the XR eye provides pose and aperture.
 * The RHI uses a negative Vulkan viewport height, so this path keeps +Y up. */
static inline int tsfp_xbox_eye_transform_depth(XrMatrix4x4f *out, const XrMatrix4x4f *game_projection,
                                                const XrPosef *anchor, const XrView *eye,
                                                float meters_per_unit, float near_z, float far_z)
{
    if (!isfinite(meters_per_unit) || meters_per_unit <= 0 ||
        !(near_z > 0 && far_z > near_z && isfinite(far_z)))
        return 0;

    XrMatrix4x4f game_from_clip, meters_from_game, meters_from_clip;
    XrMatrix4x4f_Invert(&game_from_clip, game_projection);
    const float z_scale = game_projection->m[11] < 0 ? meters_per_unit : -meters_per_unit;
    XrMatrix4x4f_CreateScale(&meters_from_game, meters_per_unit, meters_per_unit, z_scale);
    XrMatrix4x4f_Multiply(&meters_from_clip, &meters_from_game, &game_from_clip);
    const XrMatrix4x4f eye_from_meters = tsfp_eye_clip_for_api(anchor, eye, near_z, far_z, GRAPHICS_D3D);
    XrMatrix4x4f_Multiply(out, &eye_from_meters, &meters_from_clip);
    for (unsigned i = 0; i < 16; ++i)
        if (!isfinite(out->m[i]))
            return 0;
    return 1;
}

static inline int tsfp_xbox_eye_transform(XrMatrix4x4f *out, const XrMatrix4x4f *game_projection,
                                          const XrPosef *anchor, const XrView *eye,
                                          float meters_per_unit)
{
    float near_z, far_z;
    return tsfp_xbox_camera_planes(game_projection, &near_z, &far_z) &&
        tsfp_xbox_eye_transform_depth(out, game_projection, anchor, eye, meters_per_unit,
                                     near_z * meters_per_unit, far_z * meters_per_unit);
}

/* Put the original sprite on a plane across the turret's aim direction.
 * The guest's depth was for a 2D overlay. Use one distance for both eyes
 * while preserving the lens's angular size and center. */
static inline XrMatrix4x4f tsfp_turret_reticle_transform(const XrMatrix4x4f *game,
                                                        const XrPosef *anchor, const XrView *eye,
                                                        const XrVector2f *center)
{
    const float distance = 10.0f;
    const float z = copysignf(distance, game->m[11]);
    const float w = game->m[11] * z;
    XrMatrix4x4f panel = {{0}};
    panel.m[0] = w / game->m[0];
    panel.m[5] = w / game->m[5];
    panel.m[12] = center ? center->x : -game->m[8] * z / game->m[0];
    panel.m[13] = center ? center->y : -game->m[9] * z / game->m[5];
    panel.m[14] = -distance;
    panel.m[15] = 1.0f;
    const XrMatrix4x4f clip_from_anchor = tsfp_eye_clip_for_api(anchor, eye, 0.1f, 100.0f, GRAPHICS_D3D);
    XrMatrix4x4f transform;
    XrMatrix4x4f_Multiply(&transform, &clip_from_anchor, &panel);
    return transform;
}

#endif
