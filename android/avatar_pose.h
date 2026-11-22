#ifndef TSFP_AVATAR_POSE_H
#define TSFP_AVATAR_POSE_H

#include "avatar_model.h"
#include "xr_linear.h"

/* Neutral Touch Plus holds from the calibration used by the working Quest
 * build. Columns use the hand-calibration.txt convention. */
static inline XrMatrix4x4f tsfp_avatar_default_hand_hold(unsigned hand)
{
    const XrMatrix4x4f holds[TSFP_AVATAR_HANDS] = {
        { .m = { 0.25125f, 0.30674f, -0.91803f, 0,
                 0.01508f, 0.94710f, 0.32058f, 0,
                 0.96780f, -0.09439f, 0.23334f, 0, 0, 0, 0, 1 } },
        { .m = { 0.06327f, 0.14477f, -0.98744f, 0,
                 -0.13432f, 0.98166f, 0.13532f, 0,
                 0.98892f, 0.12407f, 0.08155f, 0, 0, 0, 0, 1 } }
    };
    return holds[hand];
}

static inline XrVector3f tsfp_matrix_position(const XrMatrix4x4f *matrix)
{
    return (XrVector3f){ matrix->m[12], matrix->m[13], matrix->m[14] };
}

/* Convert the fixed grip orientation into a correction for an animated bone. */
static inline XrMatrix4x4f tsfp_avatar_hand_rotation(const XrMatrix4x4f *hold,
                                                   const XrMatrix4x4f *aim,
                                                   const XrMatrix4x4f *animated)
{
    XrMatrix4x4f hand_camera, animated_inverse, rotation;
    XrMatrix4x4f_Multiply(&hand_camera, aim, hold);
    XrMatrix4x4f_Invert(&animated_inverse, animated);
    XrMatrix4x4f_Multiply(&rotation, &hand_camera, &animated_inverse);
    return rotation;
}

static inline void tsfp_matrix_set_position(XrMatrix4x4f *matrix, XrVector3f position)
{
    matrix->m[12] = position.x;
    matrix->m[13] = position.y;
    matrix->m[14] = position.z;
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

/* Stretch along a segment's axis so the rendered mesh still joins its
 * neighbors when the controller moves beyond the animated arm's reach. */
static inline void tsfp_avatar_stretch_arm(XrMatrix4x4f *rotation, XrVector3f segment, float scale)
{
    if (fabsf(scale - 1.0f) < 1e-5f)
        return;
    XrVector3f_Normalize(&segment);
    XrVector3f rotated;
    XrMatrix4x4f_TransformVector3f(&rotated, rotation, &segment);
    const float axis[] = { segment.x, segment.y, segment.z };
    const float turned[] = { rotated.x, rotated.y, rotated.z };
    for (unsigned col = 0; col < 3; ++col)
        for (unsigned row = 0; row < 3; ++row)
            rotation->m[col * 4 + row] += (scale - 1.0f) * turned[row] * axis[col];
}

/* Solve in the draw palette with the wrist locked to the controller. Preserve
 * the animated lengths within reach. Outside it, keep the shoulder fixed
 * and extend the two segments together. Orient the elbow down and out. */
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
    if (!(a > 0.005f && a < 1.0f && b > 0.005f && b < 1.0f && reach < 1.5f))
        return 0;
    if (reach < 0.001f)
        axis = upper;
    XrVector3f_Normalize(&axis);
    const float margin = fminf(0.003f, 0.1f * fminf(a, b));
    const float extension = fmaxf(1.0f, (reach + margin) / (a + b));
    const float upper_length = a * extension, lower_length = b * extension;
    const float distance = fmaxf(fabsf(upper_length - lower_length) + margin, reach);
    const float along = (upper_length * upper_length - lower_length * lower_length +
                         distance * distance) / (2 * distance);
    const float away = sqrtf(fmaxf(0, upper_length * upper_length - along * along));

    const XrVector3f outward = { 0.65f * outward_x, -1, -0.15f };
    XrVector3f side, parallel, elbow_offset, new_elbow;
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
    XrVector3f_Add(&new_elbow, &shoulder, &elbow_offset);

    XrVector3f forearm, bend_normal;
    XrVector3f_Sub(&forearm, &target, &new_elbow);
    XrVector3f_Cross(&bend_normal, &axis, &side);
    const XrQuaternionf upper_turn = tsfp_arm_rotation(upper, elbow_offset, bend_normal);
    const float forearm_length = XrVector3f_Length(&forearm);
    const XrQuaternionf lower_turn = tsfp_arm_rotation(lower,
        forearm_length > 1e-5f ? forearm : lower, bend_normal);
    XrMatrix4x4f upper_rotation, lower_rotation;
    XrMatrix4x4f_CreateFromQuaternion(&upper_rotation, &upper_turn);
    XrMatrix4x4f_CreateFromQuaternion(&lower_rotation, &lower_turn);
    tsfp_avatar_stretch_arm(&upper_rotation, upper, extension);
    tsfp_repose_bone(&bones[TSFP_ARM_UPPER], &upper_rotation, shoulder);
    tsfp_repose_bone(&bones[TSFP_ARM_ELBOW_JOINT], &lower_rotation, new_elbow);
    tsfp_avatar_stretch_arm(&lower_rotation, lower, forearm_length / b);
    tsfp_repose_bone(&bones[TSFP_ARM_FOREARM], &lower_rotation, new_elbow);
    tsfp_repose_bone(&bones[TSFP_ARM_WRIST_JOINT], wrist_rotation, target);
    tsfp_repose_bone(&bones[TSFP_ARM_HAND], wrist_rotation, target);
    return 1;
}

#endif
