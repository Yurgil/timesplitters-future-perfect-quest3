#include "stereo_math.h"
#include "avatar_pose.h"
#include "stereo_probe.h"
#include <string.h>

static XrVector3f ndc(const XrMatrix4x4f *matrix, float x, float y, float z)
{
    XrVector4f point = { x, y, z, 1.0f }, clip;
    XrMatrix4x4f_TransformVector4f(&clip, matrix, &point);
    assert(clip.w > 0.0f);
    return (XrVector3f){ clip.x / clip.w, clip.y / clip.w, clip.z / clip.w };
}

int main(void)
{
    /* Mounting and both mounted cameras must bypass snap and weapon flicks. */
    const uint32_t turret_modes[] = {0x20u, 0x40u, 0x80u, 0x10000u};
    for (unsigned i = 0; i < sizeof turret_modes / sizeof turret_modes[0]; ++i) {
        const uint32_t mode = turret_modes[i];
        const int mounted = tsfp_turret_camera_mode(mode);
        assert(mounted);
        assert(tsfp_turret_uses_aim_bindings(mode) == (mode == 0x40u || mode == 0x80u));
        TsfpRightStickState stick = { .snap_armed = 1, .weapon_armed = 1 };
        const TsfpRightStickInput aim = tsfp_right_stick((XrVector2f){.8f, -.9f}, mounted, 45, &stick);
        assert(aim.look.x == .8f && aim.look.y == -.9f && aim.snap_degrees == 0 && aim.weapon == 0);
    }
    assert(!tsfp_turret_camera_mode(2u) && !tsfp_turret_camera_mode(0x400u));

    assert(tsfp_cart_turn(12.0f, 0, 45, 1.0f / 72) == -33.0f);
    assert(tsfp_cart_turn(170.0f, 0, -30, 1.0f / 72) == -160.0f);
    assert(tsfp_cart_turn(27.0f, 0, 0, 1.0f / 72) == 27.0f);
    for (int hz = 60; hz <= 120; hz += 60) {
        float offset = 0.0f;
        for (int tick = 0; tick < hz; ++tick)
            offset = tsfp_cart_turn(offset, 1.0f, 0, 1.0f / hz);
        assert(fabsf(offset + 90.0f) < .001f);
    }

    /* A fresh installation must use the calibrated hold even when the
       character animation has rolled the wrist upside down. */
    for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand) {
        const XrMatrix4x4f hold = tsfp_avatar_default_hand_hold(hand);
        assert(XrMatrix4x4f_IsRigidBody(&hold, 0.001f));
        for (unsigned pose = 0; pose < 3; ++pose) {
            XrMatrix4x4f aim, animated, expected, correction, actual;
            XrMatrix4x4f_CreateRotation(&aim, 0.2f * pose, -0.3f * pose, 0.15f * pose);
            XrMatrix4x4f_CreateRotation(&animated, 0, 0, pose == 1 ? 3.14159265f : 0.4f);
            XrMatrix4x4f_Multiply(&expected, &aim, &hold);
            correction = tsfp_avatar_hand_rotation(&hold, &aim, &animated);
            XrMatrix4x4f_Multiply(&actual, &correction, &animated);
            for (unsigned i = 0; i < 16; ++i)
                assert(fabsf(actual.m[i] - expected.m[i]) < 1e-4f);
        }
    }
    const XrPosef anchor = {
        .orientation = { .y = 0.70710678f, .w = 0.70710678f },
        .position = { 10.0f, 1.5f, 2.0f }
    };
    const XrVector2f walk_forward = { 0, 1 };
    const XrQuaternionf straight = { .w = 1 };
    XrVector2f movement = tsfp_head_relative_stick(walk_forward, anchor.orientation, straight);
    assert(fabsf(movement.x + 1.0f) < 1e-5f && fabsf(movement.y) < 1e-5f);
    movement = tsfp_head_relative_stick(walk_forward, straight, anchor.orientation);
    assert(fabsf(movement.x - 1.0f) < 1e-5f && fabsf(movement.y) < 1e-5f);
    movement = tsfp_head_relative_stick(walk_forward, anchor.orientation, anchor.orientation);
    assert(fabsf(movement.x) < 1e-5f && fabsf(movement.y - 1.0f) < 1e-5f);

    int armed = 0;
    assert(tsfp_stick_step(1.0f, &armed) == 0);
    assert(tsfp_stick_step(0.0f, &armed) == 0);
    assert(tsfp_stick_step(0.8f, &armed) == 1);
    assert(tsfp_stick_step(1.0f, &armed) == 0);
    assert(tsfp_stick_step(-0.4f, &armed) == 0);
    assert(tsfp_stick_step(-0.2f, &armed) == 0);
    assert(tsfp_stick_step(-0.8f, &armed) == -1);

    for (int degrees = 30; degrees <= 60; degrees += 15) {
        TsfpRightStickState stick = {0};
        TsfpRightStickInput on_foot = tsfp_right_stick((XrVector2f){.9f,0}, 0, degrees, &stick);
        assert(on_foot.snap_degrees == 0);
        on_foot = tsfp_right_stick((XrVector2f){0,.9f}, 0, degrees, &stick);
        assert(on_foot.snap_degrees == 0 && on_foot.look.y == 0 && on_foot.weapon == 1);
        on_foot = tsfp_right_stick((XrVector2f){.9f,0}, 0, degrees, &stick);
        assert(on_foot.snap_degrees == degrees && on_foot.look.x == 0 && on_foot.weapon == 1);
        on_foot = tsfp_right_stick((XrVector2f){.9f,0}, 0, degrees, &stick);
        assert(on_foot.snap_degrees == 0);
        tsfp_right_stick((XrVector2f){0}, 0, degrees, &stick);
        on_foot = tsfp_right_stick((XrVector2f){-.9f,0}, 0, degrees, &stick);
        assert(on_foot.snap_degrees == -degrees);

        TsfpRightStickInput mounted = tsfp_right_stick((XrVector2f){.9f,.65f}, 1, degrees, &stick);
        assert(mounted.snap_degrees == 0 && mounted.weapon == 0 && mounted.look.x == .9f && mounted.look.y == .65f);
        mounted = tsfp_right_stick((XrVector2f){0,-.9f}, 1, degrees, &stick);
        assert(mounted.snap_degrees == 0 && mounted.weapon == 0 && mounted.look.x == 0 && mounted.look.y == -.9f);
        on_foot = tsfp_right_stick((XrVector2f){.9f,.65f}, 0, 0, &stick);
        assert(on_foot.snap_degrees == 0 && on_foot.look.x == .9f && on_foot.look.y == 0);
    }

    for (int degrees = 0; degrees <= 30; degrees += 30) {
        TsfpRightStickState stick = {0};
        tsfp_right_stick((XrVector2f){0}, 0, degrees, &stick);
        TsfpRightStickInput input = tsfp_right_stick((XrVector2f){.75f,.9f}, 0, degrees, &stick);
        assert(input.weapon == 1 && input.snap_degrees == 0 && input.look.x == 0);
        assert(tsfp_right_stick((XrVector2f){0,.9f}, 0, degrees, &stick).weapon == 1);
        assert(tsfp_right_stick((XrVector2f){0,.9f}, 0, degrees, &stick).weapon == 0);
        assert(tsfp_right_stick((XrVector2f){0,-.9f}, 0, degrees, &stick).weapon == 0);
        tsfp_right_stick((XrVector2f){0}, 0, degrees, &stick);
        assert(tsfp_right_stick((XrVector2f){0,-.9f}, 0, degrees, &stick).weapon == -1);
        assert(tsfp_right_stick((XrVector2f){0,-.9f}, 1, degrees, &stick).weapon == 0);
        assert(tsfp_right_stick((XrVector2f){0,-.9f}, 0, degrees, &stick).weapon == 0);
    }

    const XrVector3f grip = { 0.25f, -0.40f, -0.35f };
    const XrVector3f wrist = { 0.05f, -0.035f, -0.105f };
    const XrPosef model = tsfp_wrist_pose(grip, anchor.orientation, wrist);
    XrVector3f positioned;
    XrPosef_TransformVector3f(&positioned, &model, &wrist);
    assert(fabsf(positioned.x - grip.x) < 1e-5f);
    assert(fabsf(positioned.y - grip.y) < 1e-5f);
    assert(fabsf(positioned.z - grip.z) < 1e-5f);
    const XrVector3f offset = tsfp_avatar_room_offset(anchor.position, (XrVector3f){10.3f, 0.8f, 2.4f});
    assert(fabsf(offset.x - 0.21f) < 1e-5f && offset.y == 0.0f && fabsf(offset.z - 0.28f) < 1e-5f);

    /* Pitching the eyes down about the neck leaves the body in place. */
    XrPosef level = { {0, 0, 0, 1}, {0.4f, 1.6f, -0.2f} }, down;
    const XrVector3f neck = tsfp_head_pivot(&level);
    XrQuaternionf_CreateFromAxisAngle(&down.orientation, &(XrVector3f){1, 0, 0}, -1.0f);
    XrVector3f eyes_from_neck, neck_to_eyes;
    XrVector3f_Sub(&neck_to_eyes, &level.position, &neck);
    XrQuaternionf_RotateVector3f(&eyes_from_neck, &down.orientation, &neck_to_eyes);
    XrVector3f_Add(&down.position, &neck, &eyes_from_neck);
    const XrVector3f still = tsfp_avatar_room_offset(tsfp_head_pivot(&level), tsfp_head_pivot(&down));
    assert(fabsf(still.x) < 1e-5f && fabsf(still.z) < 1e-5f);
    assert(down.position.z < level.position.z - 0.02f);

    /* The same fixed grip-to-aim rotation works when Touch first appears
       in a different orientation. These pose pairs are from a Quest 3. */
    XrQuaternionf touch_grip = { 0.313f, -0.311f, -0.061f, 0.895f };
    XrQuaternionf touch_aim = { -0.198f, 0.167f, 0.941f, 0.216f };
    XrVector3f forward = {0, 0, -1}, recovered_ray, aim_ray;
    XrQuaternionf_Normalize(&touch_grip);
    XrQuaternionf_Normalize(&touch_aim);
    XrQuaternionf recovered = tsfp_touch_aim_from_grip(touch_grip, TSFP_AVATAR_RIGHT);
    XrQuaternionf_RotateVector3f(&recovered_ray, &recovered, &forward);
    XrQuaternionf_RotateVector3f(&aim_ray, &touch_aim, &forward);
    assert(XrVector3f_Dot(&recovered_ray, &aim_ray) > 0.9999f);

    /* A pelvis with spine and two leg branches, then a chest with a head
       and two arms. Nodes can be siblings without being adjacent. */
    uint8_t nodes[20 * TSFP_XBR_NODE_BYTES];
    memset(nodes, TSFP_XBR_NO_NODE, sizeof nodes);
    const unsigned parents[] = { 255, 0, 1, 2, 2, 2, 0, 6, 0, 8 };
    for (unsigned i = 0; i < sizeof parents / sizeof parents[0]; ++i)
        nodes[i * TSFP_XBR_NODE_BYTES + TSFP_XBR_PARENT] = parents[i];
    nodes[10 * TSFP_XBR_NODE_BYTES + TSFP_XBR_PARENT] = 0;
    nodes[10 * TSFP_XBR_NODE_BYTES + TSFP_XBR_TYPE] = 3;
    nodes[11 * TSFP_XBR_NODE_BYTES + TSFP_XBR_PARENT] = 2;
    nodes[11 * TSFP_XBR_NODE_BYTES + TSFP_XBR_TYPE] = 3;
    nodes[TSFP_XBR_CHILD] = 1;
    nodes[TSFP_XBR_NODE_BYTES + TSFP_XBR_CHILD] = 2;
    nodes[TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 6;
    nodes[6 * TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 10;
    nodes[10 * TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 8;
    nodes[2 * TSFP_XBR_NODE_BYTES + TSFP_XBR_CHILD] = 3;
    nodes[3 * TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 4;
    nodes[4 * TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 11;
    nodes[11 * TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 5;
    assert(tsfp_avatar_spine_end(nodes, 20) == 2);
    assert(tsfp_avatar_hide_head(nodes, 20, 3, 3));
    assert(!tsfp_avatar_hide_head(nodes, 20, 3, 0));
    assert(!tsfp_avatar_hide_head(nodes, 20, 3, 2));
    assert(!tsfp_avatar_hide_head(nodes, 20, 3, 4));
    assert(!tsfp_avatar_hide_head(nodes, 20, 3, 5));
    assert(!tsfp_avatar_hide_head(nodes, 20, 3, 7));
    assert(!tsfp_avatar_hide_head(nodes, 20, 3, 9));

    uint8_t rig_nodes[20 * TSFP_XBR_NODE_BYTES];
    memset(rig_nodes, TSFP_XBR_NO_NODE, sizeof rig_nodes);
    rig_nodes[TSFP_XBR_CHILD] = 1;
    rig_nodes[TSFP_XBR_NODE_BYTES + TSFP_XBR_PARENT] = 0;
    rig_nodes[TSFP_XBR_NODE_BYTES + TSFP_XBR_TYPE] = 0;
    rig_nodes[TSFP_XBR_NODE_BYTES + TSFP_XBR_CHILD] = 2;
    rig_nodes[TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 3;
    rig_nodes[2 * TSFP_XBR_NODE_BYTES + TSFP_XBR_PARENT] = 1;
    rig_nodes[2 * TSFP_XBR_NODE_BYTES + TSFP_XBR_TYPE] = 0;
    rig_nodes[3 * TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 11;
    const uint8_t types[] = {2, 0, 1, 0, 2, 0, 2, 0};
    for (unsigned side = 0; side < TSFP_AVATAR_HANDS; ++side) {
        const unsigned start = 3 + 8 * side;
        for (unsigned i = 0; i < sizeof types; ++i) {
            const unsigned index = start + i;
            uint8_t *node = rig_nodes + index * TSFP_XBR_NODE_BYTES;
            node[TSFP_XBR_TYPE] = types[i];
            node[TSFP_XBR_PARENT] = i ? index - 1 : 0;
            node[TSFP_XBR_CHILD] = i + 1 < sizeof types ? index + 1 : TSFP_XBR_NO_NODE;
            node[TSFP_XBR_MATRIX] = index;
        }
    }
    TsfpAvatarArm arms[TSFP_AVATAR_HANDS];
    const unsigned head = tsfp_avatar_arm_rig(rig_nodes, 20, 0, 20, arms);
    assert(head == 1);
    assert(tsfp_avatar_hide_head(rig_nodes, 20, head, 2));
    assert(!tsfp_avatar_hide_head(rig_nodes, 20, head, 10));
    assert(arms[TSFP_AVATAR_RIGHT].matrix[TSFP_ARM_HAND] == 10);
    assert(arms[TSFP_AVATAR_LEFT].matrix[TSFP_ARM_HAND] == 18);

    /* Disguise and arcade rigs can use helper nodes where the armored
       Cortez model has skinning connectors. Head order may differ too. */
    rig_nodes[3 * TSFP_XBR_NODE_BYTES + TSFP_XBR_TYPE] = 1;
    rig_nodes[7 * TSFP_XBR_NODE_BYTES + TSFP_XBR_TYPE] = 1;
    rig_nodes[17 * TSFP_XBR_NODE_BYTES + TSFP_XBR_TYPE] = 1;
    rig_nodes[TSFP_XBR_CHILD] = 3;
    rig_nodes[3 * TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 1;
    rig_nodes[TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 11;
    assert(tsfp_avatar_arm_rig(rig_nodes, 20, 0, 20, arms) == head);
    const TsfpAvatarArm *right_arm = &arms[TSFP_AVATAR_RIGHT], *left_arm = &arms[TSFP_AVATAR_LEFT];
    assert(right_arm->matrix[TSFP_ARM_ELBOW_JOINT] == TSFP_XBR_NO_NODE);
    assert(tsfp_avatar_pose_matrix(right_arm, TSFP_ARM_ELBOW_JOINT) == 8);
    assert(tsfp_avatar_pose_matrix(right_arm, TSFP_ARM_WRIST_JOINT) == 9);
    assert(left_arm->matrix[TSFP_ARM_WRIST_JOINT] == TSFP_XBR_NO_NODE);
    assert(tsfp_avatar_pose_matrix(left_arm, TSFP_ARM_WRIST_JOINT) == 18);
    rig_nodes[18 * TSFP_XBR_NODE_BYTES + TSFP_XBR_MATRIX] = 10;
    assert(!tsfp_avatar_arm_rig(rig_nodes, 20, 0, 20, arms));
    rig_nodes[18 * TSFP_XBR_NODE_BYTES + TSFP_XBR_MATRIX] = 18;
    rig_nodes[11 * TSFP_XBR_NODE_BYTES + TSFP_XBR_NEXT] = 3;
    assert(!tsfp_avatar_arm_rig(rig_nodes, 20, 0, 20, arms));

    XrMatrix4x4f bones[TSFP_ARM_MATRICES], identity;
    XrMatrix4x4f_CreateIdentity(&identity);
    for (unsigned i = 0; i < TSFP_ARM_MATRICES; ++i)
        XrMatrix4x4f_CreateTranslation(&bones[i], 0, 0, 0.3f * ((i + 1) / 2));
    const XrVector3f target = { -0.25f, -0.05f, 0.30f };
    assert(tsfp_avatar_pose_arm(bones, -1.0f, target, &identity));
    const XrVector3f new_elbow = tsfp_matrix_position(&bones[TSFP_ARM_ELBOW_JOINT]);
    const XrVector3f new_wrist = tsfp_matrix_position(&bones[TSFP_ARM_HAND]);
    XrVector3f error, lower;
    XrVector3f_Sub(&error, &target, &new_wrist);
    XrVector3f_Sub(&lower, &new_wrist, &new_elbow);
    assert(XrVector3f_Length(&error) < 1e-5f);
    assert(fabsf(XrVector3f_Length(&new_elbow) - 0.3f) < 1e-5f);
    assert(fabsf(XrVector3f_Length(&lower) - 0.3f) < 1e-5f);
    /* Beyond the animated reach, the hand and both rendered segments
     * still meet at the joints. The shoulder remains attached. */
    for (unsigned i = 0; i < TSFP_ARM_MATRICES; ++i)
        XrMatrix4x4f_CreateTranslation(&bones[i], 0, 0, 0.3f * ((i + 1) / 2));
    const XrVector3f extended = {0.18f, -0.2f, -0.86f};
    assert(tsfp_avatar_pose_arm(bones, -1.0f, extended, &identity));
    const XrVector3f arm_tip = {0, 0, 0.30f};
    const XrVector3f elbow_joint = tsfp_matrix_position(&bones[TSFP_ARM_ELBOW_JOINT]);
    const XrVector3f hand_joint = tsfp_matrix_position(&bones[TSFP_ARM_HAND]);
    XrVector3f endpoint;
    XrMatrix4x4f_TransformVector3f(&endpoint, &bones[TSFP_ARM_UPPER], &arm_tip);
    XrVector3f_Sub(&error, &endpoint, &elbow_joint);
    assert(XrVector3f_Length(&error) < 1e-5f);
    XrMatrix4x4f_TransformVector3f(&endpoint, &bones[TSFP_ARM_FOREARM], &arm_tip);
    XrVector3f_Sub(&error, &endpoint, &hand_joint);
    assert(XrVector3f_Length(&error) < 1e-5f);
    XrVector3f_Sub(&error, &hand_joint, &extended);
    assert(XrVector3f_Length(&error) < 1e-5f);
    const XrVector3f shoulder_joint = tsfp_matrix_position(&bones[TSFP_ARM_UPPER]);
    assert(XrVector3f_Length(&shoulder_joint) < 1e-5f);
    assert(!tsfp_avatar_pose_arm(bones, -1.0f, (XrVector3f){0, NAN, 0}, &identity));

    /* Fix the hand to a local-space grip with yaw and body setback. */
    for (unsigned i = 0; i < TSFP_ARM_MATRICES; ++i)
        XrMatrix4x4f_CreateTranslation(&bones[i], 0.24f, -0.22f, 0.10f - 0.285f * ((i + 1) / 2));
    XrPosef body = anchor, camera_from_local;
    body.position.x += 0.18f;
    body.position.z += 0.14f;
    XrPosef_Invert(&camera_from_local, &body);
    const XrVector3f local_grip = {9.65f, 1.13f, 1.85f};
    XrVector3f target_in_camera, wrist_in_local;
    XrPosef_TransformVector3f(&target_in_camera, &camera_from_local, &local_grip);
    assert(tsfp_avatar_pose_arm(bones, 1.0f, target_in_camera, &identity));
    const XrVector3f wrist_in_camera = tsfp_matrix_position(&bones[TSFP_ARM_WRIST_JOINT]);
    XrPosef_TransformVector3f(&wrist_in_local, &body, &wrist_in_camera);
    XrVector3f_Sub(&error, &wrist_in_local, &local_grip);
    assert(XrVector3f_Length(&error) < 1e-5f);

    /* A short arcade limb still follows a reachable controller target. */
    const float short_joints[] = { 0.0f, 0.033f, 0.033f, 0.051f, 0.051f };
    for (unsigned i = 0; i < TSFP_ARM_MATRICES; ++i)
        XrMatrix4x4f_CreateTranslation(&bones[i], 0, 0, short_joints[i]);
    const XrVector3f short_target = { 0.034f, 0, -0.018f };
    assert(tsfp_avatar_pose_arm(bones, 1.0f, short_target, &identity));
    const XrVector3f short_wrist = tsfp_matrix_position(&bones[TSFP_ARM_HAND]);
    XrVector3f_Sub(&error, &short_target, &short_wrist);
    assert(XrVector3f_Length(&error) < 1e-5f);

    const XrFovf fov = {
        .angleLeft = -0.8f, .angleRight = 0.8f,
        .angleUp = 0.8f, .angleDown = -0.8f
    };
    XrView left = { .pose = anchor, .fov = fov };
    XrView right = left;
    /* The anchor turns +X (right) toward -Z in local space. */
    left.pose.position.z += 0.032f;
    right.pose.position.z -= 0.032f;
    XrMatrix4x4f l = tsfp_eye_clip(&anchor, &left, 0.1f, 10.0f);
    XrMatrix4x4f r = tsfp_eye_clip(&anchor, &right, 0.1f, 10.0f);
    float left_near = ndc(&l, 0.0f, 0.0f, -1.0f).x;
    float right_near = ndc(&r, 0.0f, 0.0f, -1.0f).x;
    assert(left_near > 0.0f && right_near < 0.0f);
    float far_disparity = ndc(&l, 0.0f, 0.0f, -2.0f).x - ndc(&r, 0.0f, 0.0f, -2.0f).x;
    assert(fabsf((left_near - right_near) - 2.0f * far_disparity) < 1e-5f);
    assert(ndc(&l, 0.0f, 1.0f, -2.0f).y < 0.0f);
    assert(fabsf(ndc(&l, 0.0f, 0.0f, -0.1f).z) < 1e-5f);
    assert(fabsf(ndc(&l, 0.0f, 0.0f, -10.0f).z - 1.0f) < 1e-5f);

    right.pose = anchor;
    right.fov.angleLeft = -0.6f;
    r = tsfp_eye_clip(&anchor, &right, 0.1f, 10.0f);
    assert(ndc(&r, 0.0f, 0.0f, -2.0f).x < 0.0f);

    /* At a 90-degree head turn, the stereo frustum must still include
       both eyes' near and distant corners. */
    const XrPosef origin = { .orientation = { .w = 1.0f } };
    XrView cull_eyes[3] = { left, right,
        { .pose = {anchor.orientation, {-.45f, .05f, -.8f}}, .fov = {-.08f,.08f,.08f,-.08f} } };
    cull_eyes[1].pose.position.z -= 0.032f;
    XrMatrix4x4f visibility;
    assert(tsfp_stereo_visibility_clip(&visibility, &origin, &anchor, cull_eyes, 2, 0.06f, 100.0f));
    assert(tsfp_stereo_visibility_clip(&visibility, &origin, &anchor, cull_eyes, 3, 0.06f, 100.0f));
    for (unsigned eye = 0; eye < 3; ++eye) {
        const XrFovf aperture = cull_eyes[eye].fov;
        const float sides[] = { aperture.angleLeft, aperture.angleRight };
        const float heights[] = { aperture.angleDown, aperture.angleUp };
        for (unsigned corner = 0; corner < 4; ++corner) {
            for (float distance = 0.06f; distance <= 60.0f; distance *= 100.0f) {
                const XrVector3f eye_point = { distance * tanf(sides[corner & 1]),
                    distance * tanf(heights[corner >> 1]), -distance };
                XrVector3f world;
                XrPosef_TransformVector3f(&world, &cull_eyes[eye].pose, &eye_point);
                const XrVector3f point = ndc(&visibility, world.x, world.y, world.z);
                assert(fabsf(point.x) <= 1.0001f && fabsf(point.y) <= 1.0001f);
                assert(point.z >= -0.0001f && point.z <= 1.0001f);
            }
        }
    }

    /* Near the edge, an object may be visible to only one eye. */
    XrView cull_left = { .pose = origin, .fov = fov };
    XrView cull_right = cull_left;
    cull_left.pose.position.x = -0.032f;
    cull_right.pose.position.x = 0.032f;
    const XrMatrix4x4f cull_l = tsfp_eye_clip_for_api(&origin, &cull_left, 0.06f, 100.0f, GRAPHICS_D3D);
    const XrMatrix4x4f cull_r = tsfp_eye_clip_for_api(&origin, &cull_right, 0.06f, 100.0f, GRAPHICS_D3D);
    XrVector4f lp[TSFP_FRUSTUM_PLANES], rp[TSFP_FRUSTUM_PLANES];
    tsfp_clip_planes(lp, &cull_l);
    tsfp_clip_planes(rp, &cull_r);
    const XrVector3f edge_lo = { 0.211f, -0.005f, -0.201f };
    const XrVector3f edge_hi = { 0.219f, 0.005f, -0.199f };
    assert(tsfp_box_outside_frustum(lp, edge_lo, edge_hi));
    assert(!tsfp_box_outside_frustum(rp, edge_lo, edge_hi));
    const XrVector3f behind = { 0.0f, 0.0f, 1.0f };
    assert(tsfp_box_outside_frustum(lp, behind, behind));
    assert(tsfp_box_outside_frustum(rp, behind, behind));

    /* A 10-to-1000-unit Xbox frustum corresponds to 0.1-to-10 meters. */
    const float focal = 1.0f / tanf(0.8f);
    const XrMatrix4x4f xbox = { .m = {
        focal, 0, 0, 0, 0, focal, 0, 0,
        0, 0, 1000.0f / 990.0f, 1, 0, 0, -10000.0f / 990.0f, 0
    } };
    XrView center = { .pose = anchor, .fov = fov };
    XrMatrix4x4f transform;
    assert(tsfp_xbox_eye_transform(&transform, &xbox, &anchor, &center, 0.01f));
    for (unsigned i = 0; i < 16; ++i)
        assert(fabsf(transform.m[i] / transform.m[15] - (i % 5 == 0 ? 1.0f : 0.0f)) < 1e-5f);

    XrMatrix4x4f l_world, r_world;
    right.fov = fov;
    right.pose.position.z -= 0.032f;
    assert(tsfp_xbox_eye_transform(&transform, &xbox, &anchor, &left, 0.01f));
    XrMatrix4x4f_Multiply(&l_world, &transform, &xbox);
    assert(tsfp_xbox_eye_transform(&transform, &xbox, &anchor, &right, 0.01f));
    XrMatrix4x4f_Multiply(&r_world, &transform, &xbox);
    assert(ndc(&l_world, 0.0f, 0.0f, 100.0f).x > 0.0f);
    assert(ndc(&r_world, 0.0f, 0.0f, 100.0f).x < 0.0f);
    assert(ndc(&l_world, 0.0f, 10.0f, 100.0f).y > 0.0f);
    assert(fabsf(ndc(&l_world, 0.0f, 0.0f, 10.0f).z) < 1e-5f);
    assert(fabsf(ndc(&l_world, 0.0f, 0.0f, 1000.0f).z - 1.0f) < 1e-5f);

    XrMatrix4x4f us_xbox = xbox;
    us_xbox.m[10] = -us_xbox.m[10];
    us_xbox.m[11] = -1.0f;
    assert(tsfp_xbox_eye_transform(&transform, &us_xbox, &anchor, &left, 0.01f));
    XrMatrix4x4f_Multiply(&l_world, &transform, &us_xbox);
    assert(ndc(&l_world, 0.0f, 0.0f, -100.0f).x > 0.0f);
    assert(fabsf(ndc(&l_world, 0.0f, 0.0f, -10.0f).z) < 1e-5f);
    assert(fabsf(ndc(&l_world, 0.0f, 0.0f, -1000.0f).z - 1.0f) < 1e-5f);

    /* CPU-projected ghost bounds and GPU-projected world geometry must agree
     * in both eyes, including the depth used while rendering to scratch. */
    XrMatrix4x4f game_view;
    const XrPosef game_camera = {{0,.6f,0,.8f}, {.5f,.13f,-.7f}};
    XrMatrix4x4f_CreateFromRigidTransform(&game_view, &game_camera);
    const XrView ghost_eyes[] = {left, right};
    XrVector3f centers[2];
    for (unsigned eye = 0; eye < 2; ++eye) {
        XrMatrix4x4f view, projection, clip, game_clip, eye_clip;
        tsfp_xbox_eye_camera(&game_view, &anchor, &ghost_eyes[eye], .06f, 1000.f, &view, &projection, &clip);
        XrMatrix4x4f_Multiply(&game_clip, &us_xbox, &game_view);
        assert(tsfp_xbox_eye_transform_depth(&transform, &us_xbox, &anchor, &ghost_eyes[eye],
                                            1.0f, .06f, 1000.f));
        XrMatrix4x4f_Multiply(&eye_clip, &transform, &game_clip);
        for (unsigned i = 0; i < 16; ++i)
            assert(fabsf(clip.m[i] - eye_clip.m[i]) < 1e-5f);
        centers[eye] = ndc(&clip, 3.0f, 0.1f, -1.0f);
    }
    assert(fabsf(centers[0].x - centers[1].x) > .005f);

    /* A turned head and different eye apertures must still put the reticle
     * at the same point on the world camera's center ray. */
    XrMatrix4x4f turret_lens = us_xbox;
    turret_lens.m[8] = 0.04f;
    turret_lens.m[9] = -0.02f;
    XrView turret_eyes[2] = {left, right};
    for (unsigned eye = 0; eye < 2; ++eye) {
        XrView *view = &turret_eyes[eye];
        XrQuaternionf_CreateFromAxisAngle(&view->pose.orientation, &(XrVector3f){0, 1, 0}, 1.8f);
        view->pose.position.y += 0.1f;
        view->fov.angleLeft += eye == 0 ? -0.05f : 0.03f;
        view->fov.angleUp += eye == 0 ? 0.03f : -0.02f;
        const XrMatrix4x4f reticle = tsfp_turret_reticle_transform(&turret_lens, &anchor, view, NULL);
        XrMatrix4x4f eye_from_clip, eye_from_turret;
        assert(tsfp_xbox_eye_transform(&eye_from_clip, &turret_lens, &anchor, view, 1.0f));
        XrMatrix4x4f_Multiply(&eye_from_turret, &eye_from_clip, &turret_lens);
        const XrVector3f target = ndc(&eye_from_turret, 10.0f * turret_lens.m[8] / turret_lens.m[0],
                                     10.0f * turret_lens.m[9] / turret_lens.m[5], -10.0f);
        const XrVector3f sprite = ndc(&reticle, 0, 0, 0);
        assert(fabsf(sprite.x - target.x) < 1e-5f && fabsf(sprite.y - target.y) < 1e-5f);
        assert(fabsf(ndc(&reticle, 0, 0, 1).z - sprite.z) < 1e-5f);

        /* Battle Mech shares the camera bore. The final cockpit fires
         * from the midpoint of two barrels below the camera. */
        const XrVector2f mech_centers[] = {{0, 0}, {0.0f, -0.5f}, {.12f, -.36f}};
        for (unsigned i = 0; i < sizeof mech_centers / sizeof mech_centers[0]; ++i) {
            const XrVector2f *center = &mech_centers[i];
            const XrMatrix4x4f aim_panel = tsfp_turret_reticle_transform(&turret_lens, &anchor, view, center);
            const XrVector3f shot = ndc(&eye_from_turret, center->x, center->y, -10.0f);
            const XrVector3f mark = ndc(&aim_panel, 0, 0, 0);
            assert(fabsf(mark.x - shot.x) < 1e-5f && fabsf(mark.y - shot.y) < 1e-5f);
            assert(fabsf(sprite.y - mark.y) > 1e-3f);
        }
    }

    XrMatrix4x4f viewmodel = us_xbox;
    viewmodel.m[10] = -1.01f;
    viewmodel.m[14] = -1.01f;
    float world_near, world_far;
    assert(tsfp_xbox_camera_planes(&us_xbox, &world_near, &world_far));
    assert(tsfp_xbox_eye_transform_depth(&transform, &viewmodel, &anchor, &left,
                                        0.01f, world_near * 0.01f, world_far * 0.01f));
    XrMatrix4x4f tracked;
    XrMatrix4x4f_Multiply(&tracked, &transform, &viewmodel);
    assert(fabsf(ndc(&tracked, 0.0f, 0.0f, -50.0f).z -
                 ndc(&l_world, 0.0f, 0.0f, -50.0f).z) < 1e-5f);
    /* Close geometry uses the same depth in both passes, even though the
       weapon's original lens would have clipped it near the controller. */
    assert(tsfp_xbox_eye_transform_depth(&transform, &viewmodel, &anchor, &center, 1.0f, 0.06f, 1800.0f));
    XrMatrix4x4f_Multiply(&tracked, &transform, &viewmodel);
    assert(ndc(&tracked, 0.0f, 0.0f, -0.09f).z > 0.0f);

    /* The first-person viewport places its whole depth range within the
       first one percent of the original depth buffer. */
    XrMatrix4x4f weapon_projection = viewmodel;
    weapon_projection.m[10] = -1800.0f / (1800.0f - 0.15f);
    weapon_projection.m[14] = 0.15f * weapon_projection.m[10];
    XrMatrix4x4f packed = weapon_projection;
    tsfp_xbox_pack_depth(&packed, 32767.0f / 8388608.0f, 32767.0f / 8388608.0f);
    const XrPosef weapon_pose = tsfp_wrist_pose((XrVector3f){ 0.25f, -0.24f, -0.25f },
                                               anchor.orientation, (XrVector3f){ 0.0f, -0.11f, -0.44f });
    assert(tsfp_xbox_eye_transform_depth(&transform, &packed, &weapon_pose, &left, 1.0f, 0.06f, 1800.0f));
    XrMatrix4x4f unpacked;
    XrMatrix4x4f_Multiply(&unpacked, &transform, &packed);
    assert(tsfp_xbox_eye_transform_depth(&transform, &weapon_projection, &weapon_pose, &left, 1.0f, 0.06f, 1800.0f));
    XrMatrix4x4f_Multiply(&tracked, &transform, &weapon_projection);
    const XrVector3f expected_weapon = ndc(&tracked, 0.12f, -0.08f, -0.44f);
    const XrVector3f decoded_weapon = ndc(&unpacked, 0.12f, -0.08f, -0.44f);
    assert(fabsf(decoded_weapon.x - expected_weapon.x) < 1e-5f);
    assert(fabsf(decoded_weapon.y - expected_weapon.y) < 1e-5f);
    assert(fabsf(decoded_weapon.z - expected_weapon.z) < 1e-5f);

    XrMatrix4x4f ortho;
    XrMatrix4x4f_CreateIdentity(&ortho);
    assert(!tsfp_xbox_eye_transform(&transform, &ortho, &anchor, &left, 0.01f));
    assert(!tsfp_xbox_eye_transform(&transform, &xbox, &anchor, &left, 0.0f));
    return 0;
}
