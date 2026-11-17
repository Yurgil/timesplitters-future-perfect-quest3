#ifndef TSFP_AVATAR_POSE_H
#define TSFP_AVATAR_POSE_H

#include "avatar_model.h"
#include "xr_linear.h"

static inline XrVector3f tsfp_matrix_position(const XrMatrix4x4f *matrix)
{
    return (XrVector3f){ matrix->m[12], matrix->m[13], matrix->m[14] };
}

static inline void tsfp_matrix_set_position(XrMatrix4x4f *matrix, XrVector3f position)
{
    matrix->m[12] = position.x;
    matrix->m[13] = position.y;
    matrix->m[14] = position.z;
}

/* Recover the transform used by the game to compose the model palette into
 * the draw palette. The same bone can include animation scale in both. */
static inline int tsfp_avatar_palette_basis(XrMatrix4x4f *camera_from_model,
                                            const XrMatrix4x4f *model_bone,
                                            const XrMatrix4x4f *camera_bone)
{
    for (unsigned i = 0; i < 16; ++i)
        if (!isfinite(model_bone->m[i]) || !isfinite(camera_bone->m[i]))
            return 0;
    if (!XrMatrix4x4f_IsAffine(model_bone, 1e-4f) ||
        !XrMatrix4x4f_IsAffine(camera_bone, 1e-4f))
        return 0;
    const float *m = model_bone->m;
    const XrVector3f x = {m[0], m[1], m[2]}, y = {m[4], m[5], m[6]}, z = {m[8], m[9], m[10]};
    XrVector3f cross;
    XrVector3f_Cross(&cross, &x, &y);
    if (fabsf(XrVector3f_Dot(&cross, &z)) < 1e-5f)
        return 0;
    XrMatrix4x4f bone_from_model;
    XrMatrix4x4f_Invert(&bone_from_model, model_bone);
    XrMatrix4x4f_Multiply(camera_from_model, camera_bone, &bone_from_model);
    return XrMatrix4x4f_IsRigidBody(camera_from_model, 1e-3f);
}

static inline XrQuaternionf tsfp_arm_rotation(XrVector3f from, XrVector3f to, XrVector3f bend_normal)
{
    XrVector3f_Normalize(&from);
    XrVector3f_Normalize(&to);
    const float dot = XrVector3f_Dot(&from, &to);
    XrVector3f axis;
    if (dot < -0.9999f) {
        XrVector3f_Cross(&axis, &from, &bend_normal);
        if (XrVector3f_Length(&axis) < 0.001f) {
            const XrVector3f across = fabsf(from.x) < 0.8f ?
                (XrVector3f){1, 0, 0} : (XrVector3f){0, 1, 0};
            XrVector3f_Cross(&axis, &from, &across);
        }
        XrVector3f_Normalize(&axis);
        return (XrQuaternionf){ axis.x, axis.y, axis.z, 0 };
    }
    XrVector3f_Cross(&axis, &from, &to);
    XrQuaternionf rotation = { axis.x, axis.y, axis.z, 1.0f + dot };
    XrQuaternionf_Normalize(&rotation);
    return rotation;
}

static inline void tsfp_repose_bone(XrMatrix4x4f *bone, const XrMatrix4x4f *rotation,
                                    XrVector3f joint)
{
    XrMatrix4x4f rotated;
    XrMatrix4x4f_Multiply(&rotated, rotation, bone);
    tsfp_matrix_set_position(&rotated, joint);
    *bone = rotated;
}

/* Solve in model space. Preserve the animated limb lengths and orient the
 * elbow down and out so small wrist motions do not flip its bend.
 * outward_x is the sign of model X from the chest toward this shoulder. */
static inline int tsfp_avatar_pose_arm(XrMatrix4x4f bones[TSFP_ARM_MATRICES], float outward_x,
                                       XrVector3f target, const XrMatrix4x4f *wrist_rotation)
{
    const XrVector3f shoulder = tsfp_matrix_position(&bones[TSFP_ARM_UPPER]);
    const XrVector3f elbow = tsfp_matrix_position(&bones[TSFP_ARM_ELBOW_JOINT]);
    const XrVector3f wrist = tsfp_matrix_position(&bones[TSFP_ARM_WRIST_JOINT]);
    XrVector3f upper, lower, axis;
    XrVector3f_Sub(&upper, &elbow, &shoulder);
    XrVector3f_Sub(&lower, &wrist, &elbow);
    XrVector3f_Sub(&axis, &target, &shoulder);
    const float a = XrVector3f_Length(&upper), b = XrVector3f_Length(&lower);
    const float reach = XrVector3f_Length(&axis);
    if (!(a > 0.005f && a < 1.0f && b > 0.005f && b < 1.0f && isfinite(reach)))
        return 0;
    if (reach < 0.001f)
        axis = upper;
    XrVector3f_Normalize(&axis);
    const float margin = fminf(0.003f, 0.1f * fminf(a, b));
    const float distance = fminf(a + b - margin, fmaxf(fabsf(a - b) + margin, reach));
    const float along = (a * a - b * b + distance * distance) / (2 * distance);
    const float away = sqrtf(fmaxf(0, a * a - along * along));

    const XrVector3f outward = { 0.65f * outward_x, -1, -0.15f };
    XrVector3f side, parallel, elbow_offset, wrist_offset, new_elbow, new_wrist;
    XrVector3f_Scale(&parallel, &axis, XrVector3f_Dot(&axis, &outward));
    XrVector3f_Sub(&side, &outward, &parallel);
    if (XrVector3f_Length(&side) < 0.01f) {
        const XrVector3f back = {0, 0, -1};
        XrVector3f_Scale(&parallel, &axis, -axis.z);
        XrVector3f_Sub(&side, &back, &parallel);
    }
    XrVector3f_Normalize(&side);
    XrVector3f_Scale(&elbow_offset, &axis, along);
    XrVector3f_Scale(&parallel, &side, away);
    XrVector3f_Add(&elbow_offset, &elbow_offset, &parallel);
    XrVector3f_Scale(&wrist_offset, &axis, distance);
    XrVector3f_Add(&new_elbow, &shoulder, &elbow_offset);
    XrVector3f_Add(&new_wrist, &shoulder, &wrist_offset);

    XrVector3f forearm, bend_normal;
    XrVector3f_Sub(&forearm, &new_wrist, &new_elbow);
    XrVector3f_Cross(&bend_normal, &axis, &side);
    const XrQuaternionf upper_turn = tsfp_arm_rotation(upper, elbow_offset, bend_normal);
    const XrQuaternionf lower_turn = tsfp_arm_rotation(lower, forearm, bend_normal);
    XrMatrix4x4f upper_rotation, lower_rotation;
    XrMatrix4x4f_CreateFromQuaternion(&upper_rotation, &upper_turn);
    XrMatrix4x4f_CreateFromQuaternion(&lower_rotation, &lower_turn);
    tsfp_repose_bone(&bones[TSFP_ARM_UPPER], &upper_rotation, shoulder);
    tsfp_repose_bone(&bones[TSFP_ARM_ELBOW_JOINT], &lower_rotation, new_elbow);
    tsfp_repose_bone(&bones[TSFP_ARM_FOREARM], &lower_rotation, new_elbow);
    tsfp_repose_bone(&bones[TSFP_ARM_WRIST_JOINT], wrist_rotation, new_wrist);
    tsfp_repose_bone(&bones[TSFP_ARM_HAND], wrist_rotation, new_wrist);
    return 1;
}

#endif
