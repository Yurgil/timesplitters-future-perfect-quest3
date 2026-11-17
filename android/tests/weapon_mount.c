#include "uplink_math.h"
#include "stereo_math.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t crc32(const char *text)
{
    uint32_t crc = UINT32_MAX;
    for (; *text; ++text) {
        crc ^= (unsigned char)*text;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static const uint8_t *find_asset(const uint8_t *pak, size_t bytes, const char *name, uint32_t *size)
{
    assert(bytes >= 12 && memcmp(pak, "P5CK", 4) == 0);
    const uint32_t table = word(pak + 4), count_bytes = word(pak + 8);
    assert(table <= bytes && count_bytes <= bytes - table && count_bytes % 16 == 0);
    char path[128];
    const int length = snprintf(path, sizeof path, "assets/ts/models/guns/%s/xbox/output/%s.xbr", name, name);
    assert(length > 0 && (size_t)length < sizeof path);
    const uint32_t hash = crc32(path);
    for (uint32_t offset = 0; offset < count_bytes; offset += 16) {
        const uint8_t *entry = pak + table + offset;
        if (word(entry) != hash)
            continue;
        const uint32_t start = word(entry + 4);
        *size = word(entry + 8);
        assert(word(entry + 12) == 0 && start <= bytes && *size <= bytes - start);
        return pak + start;
    }
    return NULL;
}

static XrVector3f transformed(const XrMatrix4x4f *matrix, XrVector3f point)
{
    XrVector3f result;
    XrMatrix4x4f_TransformVector3f(&result, matrix, &point);
    return result;
}

static XrVector3f difference(XrVector3f a, XrVector3f b)
{
    XrVector3f result;
    XrVector3f_Sub(&result, &a, &b);
    return result;
}

static void near(XrVector3f actual, XrVector3f expected)
{
    XrVector3f error = difference(actual, expected);
    assert(XrVector3f_Length(&error) < .0001f);
}

static void check_uplink(void)
{
    XrMatrix4x4f lens, clip;
    XrMatrix4x4f_CreateProjection(&lens, GRAPHICS_D3D, -1, 1, 1, -1, .06f, 100.f);
    const TsfpUplinkRay ray = { {12.f, 2.f, -4.f}, {1,0,0}, {0,1,0} };
    assert(tsfp_uplink_view(&ray, &lens, &clip));
    const XrVector3f target = tsfp_uplink_point(&ray, 8.f);
    near(target, (XrVector3f){20.f, 2.f, -4.f});
    near(tsfp_uplink_ray_delta(&ray, 50.f), (XrVector3f){50.f, 0.f, 0.f});
    /* Swing the wrist while holding the object at the same distance. */
    const TsfpUplinkRay raised = {{12.2f, 2.1f, -4.f}, {0,.6f,-.8f}, {0,.8f,.6f}};
    const XrVector3f moved = tsfp_uplink_point(&raised, 8.f);
    near(moved, (XrVector3f){12.2f, 6.9f, -10.4f});
    assert(tsfp_uplink_view(&raised, &lens, &clip));
    XrVector4f raised_clip;
    XrMatrix4x4f_TransformVector4f(&raised_clip, &clip, &(XrVector4f){moved.x,moved.y,moved.z,1});
    assert(raised_clip.w > 0 && fabsf(raised_clip.x) < .0001f && fabsf(raised_clip.y) < .0001f);
    assert(tsfp_uplink_view(&ray, &lens, &clip));
    const XrVector4f samples[] = {{target.x,target.y,target.z,1}, {20,3,-4,1}, {11,2,-4,1}};
    XrVector4f center, above, behind;
    XrMatrix4x4f_TransformVector4f(&center, &clip, &samples[0]);
    XrMatrix4x4f_TransformVector4f(&above, &clip, &samples[1]);
    XrMatrix4x4f_TransformVector4f(&behind, &clip, &samples[2]);
    assert(center.w > 0 && fabsf(center.x) < .0001f && fabsf(center.y) < .0001f);
    assert(above.w > 0 && fabsf(above.y) > .9f && behind.w < 0);
    assert(!tsfp_uplink_view(&(TsfpUplinkRay){ray.origin, ray.up, ray.up}, &lens, &clip));
    uint32_t address;
    assert(tsfp_uplink_entity_address(0x400000, (7u << 22) | 123u, &address));
    assert(address == 0x400000 + 7 * 0x2e8u);
    assert(!tsfp_uplink_entity_address(0x400000, 0, &address));
    assert(!tsfp_uplink_entity_address(UINT32_MAX - 0x10, (7u << 22) | 123u, &address));
}

static void check_asset(unsigned id, const TsfpWeaponProfile *profile, const uint8_t *asset, uint32_t bytes)
{
    assert(asset && bytes > 8);
    const uint32_t header = word(asset + 4);
    assert(header <= bytes - 8);
    const uint32_t nodes = word(asset + header), matrices = word(asset + header + 4);
    assert(nodes > profile->node && nodes <= header / TSFP_XBR_NODE_BYTES);
    const uint8_t *node = asset + header - (nodes - profile->node) * TSFP_XBR_NODE_BYTES;
    assert(node[TSFP_XBR_MATRIX] < matrices && word(node) != 0);
    float bounds[6];
    memcpy(bounds, node + 0x90, sizeof bounds);
    const float grip[] = {profile->grip.x, profile->grip.y, profile->grip.z};
    for (unsigned axis = 0; axis < 3; ++axis)
        assert(fabsf(grip[axis] - bounds[axis]) <= bounds[axis + 3] + .005f);
    if (profile->mount != TSFP_WEAPON_GUN)
        return;

    /* A modeled barrel can be a child of the housing, and some XBRs have
     * a muzzle marker there rather than at the end of the housing mesh. */
    const XrVector3f from_grip = difference(profile->muzzle, profile->grip);
    assert(XrVector3f_Dot(&from_grip, &profile->forward) > .12f);
    switch (id) {
    case 1: case 3: case 4: case 6: case 7: case 8: case 9: case 10:
    case 11: case 12: case 13: case 14: case 15: case 17: case 18: case 19:
    case 20: case 21: case 31: case 38: case 39: case 62: {
        const uint32_t socket = word(node + 0x78);
        assert(socket && socket <= bytes - sizeof(XrVector3f));
        XrVector3f authored;
        memcpy(&authored, asset + socket, sizeof authored);
        const XrVector3f from_authored = difference(profile->muzzle, authored);
        assert(XrVector3f_Length(&from_authored) < .004f);
        break;
    }
    }
}

static void check_pose(unsigned id, const TsfpWeaponProfile *profile, unsigned hand, XrQuaternionf aim)
{
    const XrVector3f wrist = {hand ? .28f : -.28f, -.35f, -.51f};
    XrMatrix4x4f target;
    assert(tsfp_weapon_target(id, hand, wrist, aim, &target));
    XrVector3f forward, top;
    XrQuaternionf_RotateVector3f(&forward, &aim, &(XrVector3f){0,0,-1});
    XrQuaternionf_RotateVector3f(&top, &aim, &(XrVector3f){0,-1,0});
    XrVector3f grip = profile->grip;
    if (id == TSFP_WEAPON_SCIFI_HANDGUN && hand == TSFP_AVATAR_LEFT)
        grip.x = -grip.x;
    const XrVector3f anchor = transformed(&target, grip);
    const XrVector3f from_wrist = difference(anchor, wrist);
    XrVector3f expected_offset;
    /* Camera-space offsets for the neutral aim pose (+X left, +Y down). */
    XrVector3f neutral_offset = {0, 0, -.045f};
    switch (id) {
    case 0:  neutral_offset = (XrVector3f){.030f,  .050f, -.065f}; break;
    case 9:  neutral_offset = (XrVector3f){.030f, -.020f, -.045f}; break;
    case 12: neutral_offset = (XrVector3f){.030f,  .010f, -.125f}; break;
    case 31: neutral_offset = (XrVector3f){.030f,  .040f, -.095f}; break;
    }
    XrQuaternionf_RotateVector3f(&expected_offset, &aim, &neutral_offset);
    near(from_wrist, expected_offset);

    const XrVector3f origin = tsfp_matrix_position(&target);
    XrVector3f model_forward = difference(transformed(&target, profile->forward), origin);
    const XrVector3f model_top = difference(transformed(&target, profile->up), origin);
    XrVector3f_Normalize(&model_forward);
    near(model_forward, forward);
    assert(XrVector3f_Dot(&model_top, &top) > .1f);
    if (id == TSFP_WEAPON_FLAMETHROWER)
        near(difference(transformed(&target, (XrVector3f){0,-1,0}), origin), top);

    /* The guest can draw the same housing at a different angle, scale and
     * offset. Undo the original palette before applying the controller pose. */
    XrMatrix4x4f source, correction;
    const XrVector3f scale = {hand ? .80f : -.80f, 1.1f, .95f};
    XrMatrix4x4f_CreateTranslationRotationScale(&source, &(XrVector3f){.12f,-.10f,-.30f},
                                              &(XrQuaternionf){.5f,.5f,.5f,.5f}, &scale);
    assert(tsfp_weapon_correction(&source, &target, &correction));
    near(transformed(&correction, transformed(&source, grip)), anchor);
    const XrVector3f model_muzzle = transformed(&correction, transformed(&source, profile->muzzle));
    if (profile->mount != TSFP_WEAPON_GUN)
        return;

    XrVector3f camera_muzzle, camera_direction;
    tsfp_weapon_bore(profile, &target, &camera_muzzle, &camera_direction);
    near(camera_muzzle, model_muzzle);
    near(camera_direction, forward);
    XrMatrix4x4f world;
    const XrPosef world_pose = {{0,.70710678f,0,.70710678f}, {3,-2,7}};
    XrMatrix4x4f_CreateFromRigidTransform(&world, &world_pose);
    XrVector3f muzzle, direction;
    tsfp_weapon_world_bore(&world, camera_muzzle, camera_direction, &muzzle, &direction);
    near(muzzle, transformed(&world, model_muzzle));
    XrVector3f expected_direction;
    XrQuaternionf_RotateVector3f(&expected_direction, &world_pose.orientation, &forward);
    near(direction, expected_direction);
    XrVector3f rays[3] = {{0,0,-1}, {.05f,0,-1}, {0,.03f,-1}};
    tsfp_weapon_aim_shot(direction, rays);
    near(rays[0], expected_direction);
    assert(fabsf(XrVector3f_Dot(&rays[0], &rays[1]) - 1.f) < .0001f);
    assert(fabsf(XrVector3f_Dot(&rays[0], &rays[2]) - 1.f) < .0001f);
}

static void check_cuff_pose(const TsfpWeaponProfile *profile, unsigned hand, XrQuaternionf rotation)
{
    XrMatrix4x4f forearm, target;
    XrMatrix4x4f_CreateFromQuaternion(&forearm, &rotation);
    tsfp_matrix_set_position(&forearm, (XrVector3f){.37f,-.48f,-.23f});
    const XrVector3f wrist = transformed(&forearm, (XrVector3f){.26f,0,0});
    assert(tsfp_weapon_cuff_target(profile, wrist, &forearm, hand, &target));

    /* The cuff turns about the opening. The housing now extends across the
     * forearm, and its former top faces toward the wrist after the roll. */
    const float outside = hand == TSFP_AVATAR_RIGHT ? 1.f : -1.f;
    const XrVector3f opening = {.095f, 0, outside * .050f};
    near(transformed(&target, profile->grip), transformed(&forearm, opening));
    const XrVector3f housing = transformed(&target, (XrVector3f){0,0,profile->grip.z});
    near(housing, transformed(&forearm, (XrVector3f){opening.x, 0, opening.z - outside * .087f * .70f}));
    XrVector3f outer_face = profile->grip;
    outer_face.y += .035f;
    near(transformed(&target, outer_face), transformed(&forearm, (XrVector3f){opening.x + .035f * 1.35f, 0, opening.z}));

    XrMatrix4x4f cuff_move, map_move;
    assert(tsfp_weapon_correction(&forearm, &target, &cuff_move));
    const XrVector3f pivot = transformed(&target, profile->grip);
    tsfp_uplink_rotate_map(&cuff_move, pivot, tsfp_matrix_position(&forearm), wrist, &map_move);
    near(transformed(&map_move, transformed(&forearm, profile->grip)), pivot);
    /* Looking along forearm +X with +Y up, screen-right (+Z) turns up. */
    const XrVector3f map_edge = transformed(&forearm, (XrVector3f){0,0,profile->grip.z});
    near(transformed(&map_move, map_edge),
         transformed(&forearm, (XrVector3f){opening.x, -outside * .087f * .70f, opening.z}));
    near(transformed(&map_move, transformed(&forearm, outer_face)), transformed(&target, outer_face));
}

static void check_effect_pose(unsigned id, XrVector3f local_point)
{
    XrMatrix4x4f guest_pose, controller_pose, effect_move, lens, packed_lens, from_clip, reprojection;
    XrMatrix4x4f_CreateTranslationRotationScale(&guest_pose, &(XrVector3f){.12f,-.10f,-.30f},
                                              &(XrQuaternionf){0,1,0,0}, &(XrVector3f){1,1,1});
    assert(tsfp_weapon_target(id, TSFP_AVATAR_RIGHT, (XrVector3f){.25f,-.38f,-.45f},
                              (XrQuaternionf){.5f,.5f,.5f,.5f}, &controller_pose));
    assert(tsfp_weapon_correction(&guest_pose, &controller_pose, &effect_move));

    /* Recover camera space from the guest effect's clip coordinates. */
    XrMatrix4x4f_CreateProjection(&lens, GRAPHICS_D3D, -1, 1, 1, -1, .05f, 100.f);
    packed_lens = lens;
    tsfp_xbox_pack_depth(&packed_lens, .75f, .25f);
    XrMatrix4x4f_Invert(&from_clip, &packed_lens);
    XrMatrix4x4f_Multiply(&reprojection, &effect_move, &from_clip);
    const XrVector3f source = transformed(&guest_pose, local_point);
    const XrVector4f point = {source.x, source.y, source.z, 1};
    XrVector4f clip, recovered;
    XrMatrix4x4f_TransformVector4f(&clip, &packed_lens, &point);
    XrMatrix4x4f_TransformVector4f(&recovered, &reprojection, &clip);
    assert(fabsf(recovered.w - 1.0f) < .0001f);
    near((XrVector3f){recovered.x, recovered.y, recovered.z}, transformed(&controller_pose, local_point));
}

static void check_weapon_effects(const uint8_t *pak, size_t bytes)
{
    uint32_t size = 0;
    const uint8_t *shield = find_asset(pak, bytes, "fsniper_shield", &size);
    assert(shield && size > 8);
    const uint32_t header = word(shield + 4);
    assert(header >= 2 * TSFP_XBR_NODE_BYTES && header <= size - 8);
    assert(word(shield + header) == 2 && word(shield + header + 4) == 2);
    const TsfpWeaponProfile *rifle = tsfp_weapon_profile(TSFP_WEAPON_FUTURE_SNIPER);
    for (unsigned node = 0; node < 2; ++node) {
        XrVector3f center;
        memcpy(&center, shield + header - (2 - node) * TSFP_XBR_NODE_BYTES + 0x90, sizeof center);
        assert(fabsf(center.x - rifle->muzzle.x) < .01f && fabsf(center.y - rifle->muzzle.y) < .02f);
        assert(center.z > .9f && center.z < rifle->muzzle.z);
        check_effect_pose(TSFP_WEAPON_FUTURE_SNIPER, center);
    }
    const TsfpWeaponProfile *plasma = tsfp_weapon_profile(TSFP_WEAPON_PLASMA_MACHINE_GUN);
    check_effect_pose(TSFP_WEAPON_PLASMA_MACHINE_GUN, plasma->muzzle);
    const TsfpWeaponProfile *flame = tsfp_weapon_profile(TSFP_WEAPON_FLAMETHROWER);
    check_effect_pose(TSFP_WEAPON_FLAMETHROWER, flame->muzzle);
    const TsfpWeaponProfile *soviet = tsfp_weapon_profile(12);
    check_effect_pose(12, soviet->muzzle);
    const TsfpWeaponProfile *machinegun = tsfp_weapon_profile(31);
    check_effect_pose(31, machinegun->muzzle);
    const TsfpWeaponProfile *machinegun_alt = tsfp_weapon_profile(62);
    check_effect_pose(62, machinegun_alt->muzzle);
}

static void check_spawned_muzzle_particles(void)
{
    const XrPosef anchor = {{0,.6f,0,.8f}, {1.3f,.2f,-.7f}};
    const XrVector3f room = {.14f,-.07f,.22f};
    XrVector3f local_room;
    const XrQuaternionf inverse = {0,-.6f,0,.8f};
    XrQuaternionf_RotateVector3f(&local_room, &inverse, &room);
    XrPosef body = anchor;
    XrVector3f_Add(&body.position, &body.position, &room);

    XrMatrix4x4f lens, world_from_camera, camera_from_world;
    XrMatrix4x4f_CreateProjection(&lens, GRAPHICS_D3D, -1, 1, 1, -1, .05f, 100.f);
    tsfp_xbox_pack_depth(&lens, .75f, .25f);
    XrMatrix4x4f_CreateFromRigidTransform(&world_from_camera, &anchor);
    XrMatrix4x4f_InvertRigidBody(&camera_from_world, &world_from_camera);
    const TsfpWeaponProfile *rifle = tsfp_weapon_profile(12);
    const float yaw[] = {-.60f, 0, .60f};
    for (unsigned i = 0; i < sizeof yaw / sizeof yaw[0]; ++i) {
        const XrQuaternionf aim = {0, sinf(yaw[i]), 0, cosf(yaw[i])};
        XrMatrix4x4f target;
        assert(tsfp_weapon_target(12, TSFP_AVATAR_RIGHT, (XrVector3f){.25f,-.38f,-.45f}, aim, &target));
        const XrVector3f model_muzzle = transformed(&target, rifle->muzzle);
        XrVector3f shot_origin, shot_direction, world_origin, world_direction;
        tsfp_weapon_bore(rifle, &target, &shot_origin, &shot_direction);
        XrVector3f_Add(&shot_origin, &shot_origin, &local_room);
        tsfp_weapon_world_bore(&world_from_camera, shot_origin, shot_direction,
                               &world_origin, &world_direction);
        const XrVector3f emitter = transformed(&camera_from_world, world_origin);
        for (unsigned eye = 0; eye < 2; ++eye) {
            const XrView view = {
                .pose = {anchor.orientation, {body.position.x + (eye ? .032f : -.032f),
                                             body.position.y, body.position.z}},
                .fov = {-.9f,.8f,.8f,-.8f},
            };
            XrMatrix4x4f particle_rows;
            assert(tsfp_xbox_eye_transform_depth(&particle_rows, &lens, &anchor, &view, 1.f, .05f, 100.f));
            XrMatrix4x4f particle_clip;
            XrMatrix4x4f_Multiply(&particle_clip, &particle_rows, &lens);
            const XrMatrix4x4f model_clip = tsfp_eye_clip_for_api(&body, &view, .05f, 100.f, GRAPHICS_D3D);
            near(transformed(&particle_clip, emitter), transformed(&model_clip, model_muzzle));
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s path/to/gun.pak\n", argv[0]);
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    const long end = ftell(file);
    assert(end > 0 && fseek(file, 0, SEEK_SET) == 0);
    const size_t bytes = (size_t)end;
    uint8_t *pak = malloc(bytes);
    assert(pak && fread(pak, 1, bytes, file) == bytes);
    fclose(file);

    check_uplink();
    check_weapon_effects(pak, bytes);
    check_spawned_muzzle_particles();

    assert(tsfp_weapon_hand(TSFP_WEAPON_MAIN_SLOT, 1) == TSFP_AVATAR_RIGHT);
    assert(tsfp_weapon_hand(TSFP_WEAPON_OFF_SLOT, 0) == TSFP_AVATAR_RIGHT);
    assert(tsfp_weapon_hand(TSFP_WEAPON_OFF_SLOT, 1) == TSFP_AVATAR_LEFT);
    XrMatrix4x4f kruger_root, kruger_target;
    XrMatrix4x4f_CreateTranslation(&kruger_root, .33f, -.31f, -.48f);
    tsfp_kruger_target(&kruger_root, &kruger_target);
    near(tsfp_matrix_position(&kruger_target), (XrVector3f){.33f,-.345f,-.661f});

    unsigned passed = 0;
    for (unsigned id = 0; id < sizeof tsfp_weapon_profiles / sizeof tsfp_weapon_profiles[0]; ++id) {
        const TsfpWeaponProfile *profile = tsfp_weapon_profile(id);
        if (!profile)
            continue;
        printf("%2u %s\n", id, profile->asset);
        fflush(stdout);
        uint32_t size = 0;
        const uint8_t *asset = find_asset(pak, bytes, profile->asset, &size);
        check_asset(id, profile, asset, size);
        const XrQuaternionf poses[] = {{0,0,1,0}, {.5f,.5f,.5f,.5f}, {0,0,.70710678f,.70710678f}};
        for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand)
            for (unsigned i = 0; i < sizeof poses / sizeof poses[0]; ++i)
                if (profile->mount == TSFP_WEAPON_CUFF)
                    check_cuff_pose(profile, hand, poses[i]);
                else
                    check_pose(id, profile, hand, poses[i]);
        ++passed;
    }
    assert(tsfp_weapon_profile(UINT32_MAX) == NULL && tsfp_weapon_profile(35) == NULL);
    printf("%u weapon profiles passed\n", passed);
    free(pak);
    return 0;
}
