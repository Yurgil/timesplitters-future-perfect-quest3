#ifndef TSFP_WEAPON_MOUNT_H
#define TSFP_WEAPON_MOUNT_H

#include "avatar_pose.h"

typedef enum { TSFP_WEAPON_GUN, TSFP_WEAPON_PROP, TSFP_WEAPON_CUFF } TsfpWeaponMount;

typedef struct {
    const char *asset;
    unsigned node;
    TsfpWeaponMount mount;
    XrVector3f grip, forward, up, muzzle;
} TsfpWeaponProfile;

enum { TSFP_WEAPON_OFF_SLOT, TSFP_WEAPON_MAIN_SLOT };
enum {
    TSFP_WEAPON_KRUGER = 1,
    TSFP_WEAPON_FLAMETHROWER = 10,
    TSFP_WEAPON_SCIFI_HANDGUN = 14,
    TSFP_WEAPON_FUTURE_SNIPER = 17,
    TSFP_WEAPON_PLASMA_MACHINE_GUN = 19
};

/* Landmarks in the local coordinates of each Xbox XBR housing mesh. The
 * rifle convention is not shared across assets. Empty entries are unarmed
 * or use a scripted prop rather than a standard weapon model. */
static const TsfpWeaponProfile tsfp_weapon_profiles[] = {
    [0]  = { "pistol9mm_anim", 1, TSFP_WEAPON_GUN, {0,-.080f,-.035f}, {0,0,1}, {0,1,0}, {0,.060f,.301f} },
    [1]  = { "luger_anim", 1, TSFP_WEAPON_GUN, {0,-.115f,-.112f}, {0,0,1}, {0,1,0}, {0,.0353f,.2168f} },
    [2]  = { "glock_anim", 1, TSFP_WEAPON_GUN, {0,-.079f,-.019f}, {0,0,1}, {0,1,0}, {0,.045f,.176f} },
    [3]  = { "magnum_anim", 1, TSFP_WEAPON_GUN, {0,-.106f,-.043f}, {0,0,1}, {0,1,0}, {0,.059f,.368f} },
    [4]  = { "flaregun_anim", 1, TSFP_WEAPON_GUN, {0,-.052f,0}, {.886f,-.464f,0}, {0,1,0}, {.407f,-.106f,0} },
    [5]  = { "assaultshotgun_anim", 1, TSFP_WEAPON_GUN, {0,-.099f,-.050f}, {0,0,1}, {0,1,0}, {0,.073f,.994f} },
    [6]  = { "futureshotgun_anim", 1, TSFP_WEAPON_GUN, {0,-.096f,-.046f}, {0,0,1}, {0,1,0}, {0,.102f,.637f} },
    [7]  = { "minigun_anim", 1, TSFP_WEAPON_GUN, {-.101f,.090f,-.079f}, {0,0,1}, {0,1,0}, {.006f,.042f,1.181f} },
    [8]  = { "sniperbolt_anim", 1, TSFP_WEAPON_GUN, {-.317f,0,-.101f}, {1,0,0}, {0,0,1}, {.6507f,0,0} },
    [9]  = { "sniperrifle_anim", 1, TSFP_WEAPON_GUN, {-.281f,0,-.094f}, {1,0,0}, {0,0,1}, {.6507f,0,0} },
    [TSFP_WEAPON_FLAMETHROWER] = { "flamethrower_anim", 1, TSFP_WEAPON_GUN, {0,-.042f,-.062f}, {0,0,-1}, {0,-1,0}, {0,.003f,-.835f} },
    [11] = { "harpoon_anim", 1, TSFP_WEAPON_GUN, {0,-.103f,.086f}, {0,0,1}, {0,1,0}, {0,.070f,.563f} },
    [12] = { "assaultrifle_anim", 1, TSFP_WEAPON_GUN, {0,-.140f,.093f}, {0,0,1}, {0,1,0}, {0,-.0111f,1.3223f} },
    [13] = { "electrogun_anim", 1, TSFP_WEAPON_GUN, {0,-.135f,-.025f}, {0,0,1}, {0,1,0}, {0,-.024f,.638f} },
    [TSFP_WEAPON_SCIFI_HANDGUN] = { "scifihandgun_anim", 1, TSFP_WEAPON_GUN, {-.030f,-.045f,-.068f}, {0,0,1}, {0,1,0}, {0,.0743f,.4822f} },
    [15] = { "rocketlauncher_anim", 2, TSFP_WEAPON_GUN, {0,-.093f,.023f}, {0,0,1}, {0,1,0}, {0,.089f,.617f} },
    [16] = { "homingrocketlauncher_anim", 1, TSFP_WEAPON_GUN, {.033f,-.088f,.310f}, {0,0,1}, {0,1,0}, {-.003f,.067f,.526f} },
    [TSFP_WEAPON_FUTURE_SNIPER] = { "fsniper_anim", 1, TSFP_WEAPON_GUN, {-.035f,-.052f,.025f}, {0,0,1}, {0,1,0}, {0,.0501f,1.1209f} },
    [18] = { "ghostgun_anim", 1, TSFP_WEAPON_GUN, {.059f,-.141f,-.125f}, {0,0,1}, {0,1,0}, {0,-.022f,.765f} },
    [TSFP_WEAPON_PLASMA_MACHINE_GUN] = { "plasmamachinegun_anim", 1, TSFP_WEAPON_GUN, {-.030f,-.150f,.061f}, {0,0,1}, {0,1,0}, {0,.043f,.954f} },
    [20] = { "magcharger_anim", 1, TSFP_WEAPON_GUN, {0,-.061f,.010f}, {0,0,1}, {0,1,0}, {0,-.007f,.282f} },
    [21] = { "monkeygun_anim", 1, TSFP_WEAPON_GUN, {0,-.069f,.014f}, {0,0,1}, {0,1,0}, {0,-.007f,.282f} },
    [22] = { "grenade_cl", 0, TSFP_WEAPON_PROP, {0,.002f,-.010f}, {0,0,1}, {0,1,0}, {0} },
    [23] = { "grenade_cl", 0, TSFP_WEAPON_PROP, {0,.014f,-.010f}, {0,0,1}, {0,1,0}, {0} },
    [24] = { "brick_anim", 1, TSFP_WEAPON_PROP, {0,-.025f,.009f}, {1,0,0}, {0,1,0}, {0} },
    [25] = { "proxmine_anim", 1, TSFP_WEAPON_PROP, {.037f,-.025f,-.012f}, {0,0,1}, {0,1,0}, {0} },
    [26] = { "timedmine_anim", 1, TSFP_WEAPON_PROP, {0,-.030f,0}, {0,0,1}, {0,1,0}, {0} },
    [27] = { "detonator_anim", 1, TSFP_WEAPON_PROP, {0,-.011f,.023f}, {0,0,1}, {0,1,0}, {0} },
    [28] = { "remotemine_anim", 1, TSFP_WEAPON_PROP, {0,-.025f,0}, {0,0,1}, {0,1,0}, {0} },
    [29] = { "tnt_anim", 1, TSFP_WEAPON_PROP, {.018f,-.041f,0}, {0,0,1}, {0,1,0}, {0} },
    [30] = { "nemosmg_anim", 1, TSFP_WEAPON_GUN, {0,-.120f,-.023f}, {0,0,1}, {0,1,0}, {0,.041f,.849f} },
    [31] = { "machinepistol_anim", 1, TSFP_WEAPON_GUN, {0,-.140f,-.018f}, {0,0,1}, {0,1,0}, {0,.003f,.531f} },
    [32] = { "doublebarrelshotgun_anim", 1, TSFP_WEAPON_GUN, {0,-.080f,-.060f}, {0,0,1}, {0,1,0}, {0,.030f,.590f} },
    [33] = { "baseballbat_anim", 1, TSFP_WEAPON_PROP, {.005f,.118f,-.007f}, {0,1,0}, {0,0,-1}, {0} },
    [34] = { "timegrenade", 0, TSFP_WEAPON_PROP, {0,.010f,0}, {0,0,1}, {0,1,0}, {0} },
    /* The wrist opening is at the positive-X end of the housing. */
    [36] = { "mapboy_anim", 1, TSFP_WEAPON_CUFF, {.087f,0,.0004f}, {1,0,0}, {0,1,0}, {0} },
    [37] = { "mapboy_anim", 1, TSFP_WEAPON_CUFF, {.087f,0,.0004f}, {1,0,0}, {0,1,0}, {0} },
    [38] = { "dartgun_anim", 1, TSFP_WEAPON_GUN, {0,-.072f,-.175f}, {0,0,1}, {0,1,0}, {0,.068f,.341f} },
    [39] = { "sbp500_anim", 1, TSFP_WEAPON_GUN, {-.145f,-.038f,0}, {1,0,0}, {0,1,0}, {1.038f,.005f,-.002f} },
    [62] = { "machinepistol_anim", 1, TSFP_WEAPON_GUN, {0,-.140f,-.018f}, {0,0,1}, {0,1,0}, {0,.003f,.531f} },
};

static inline const TsfpWeaponProfile *tsfp_weapon_profile(uint32_t id)
{
    return id < sizeof tsfp_weapon_profiles / sizeof tsfp_weapon_profiles[0] &&
        tsfp_weapon_profiles[id].asset ? &tsfp_weapon_profiles[id] : NULL;
}

static inline unsigned tsfp_weapon_hand(unsigned slot, int main_weapon)
{
    return slot == TSFP_WEAPON_OFF_SLOT && main_weapon ? TSFP_AVATAR_LEFT : TSFP_AVATAR_RIGHT;
}

static inline int tsfp_weapon_basis(XrMatrix4x4f *basis, XrVector3f forward, XrVector3f up)
{
    XrVector3f right;
    XrVector3f_Cross(&right, &up, &forward);
    if (!(XrVector3f_Length(&right) > .001f))
        return 0;
    XrVector3f_Normalize(&right);
    XrVector3f_Normalize(&forward);
    XrVector3f_Cross(&up, &forward, &right);
    *basis = (XrMatrix4x4f){{ right.x,right.y,right.z,0, up.x,up.y,up.z,0,
                            forward.x,forward.y,forward.z,0, 0,0,0,1 }};
    return 1;
}

static inline int tsfp_weapon_target(uint32_t id, unsigned hand, XrVector3f wrist,
                                     XrQuaternionf aim, XrMatrix4x4f *target)
{
    const TsfpWeaponProfile *profile = tsfp_weapon_profile(id);
    if (!profile)
        return 0;

    const XrVector3f aim_forward = {0,0,-1}, aim_up = {0,-1,0};
    XrVector3f forward, up;
    XrQuaternionf_RotateVector3f(&forward, &aim, &aim_forward);
    XrQuaternionf_RotateVector3f(&up, &aim, &aim_up);
    XrMatrix4x4f model_basis, hand_basis, from_model_basis;
    if (!tsfp_weapon_basis(&model_basis, profile->forward, profile->up) ||
        !tsfp_weapon_basis(&hand_basis, forward, up))
        return 0;
    XrMatrix4x4f_InvertRigidBody(&from_model_basis, &model_basis);
    XrMatrix4x4f_Multiply(target, &hand_basis, &from_model_basis);
    XrVector3f grip_point = profile->grip;
    /* This model's X axis runs across the grip. Mirror the lateral fit
     * for the left hand while keeping the model and bore orientation. */
    if (id == TSFP_WEAPON_SCIFI_HANDGUN && hand == TSFP_AVATAR_LEFT)
        grip_point.x = -grip_point.x;
    XrVector3f grip, center, offset;
    XrMatrix4x4f_TransformVector3f(&grip, target, &grip_point);
    /* Placement offsets in the controller's aim frame, in meters.
     * Keep the authored grip point on the mesh even for large adjustments. */
    XrVector3f placement = {0, 0, .045f};
    switch (id) {
    case 0:  placement = (XrVector3f){.030f, -.050f, .065f}; break; /* Pistol 9mm */
    case 9:  placement = (XrVector3f){.030f,  .020f, .045f}; break; /* Sniper Rifle */
    case 12: placement = (XrVector3f){.030f, -.010f, .125f}; break; /* Soviet Rifle */
    case 31: placement = (XrVector3f){.030f, -.040f, .095f}; break; /* Machine Gun */
    }
    XrMatrix4x4f_TransformVector3f(&offset, &hand_basis, &placement);
    XrVector3f_Add(&center, &wrist, &offset);
    XrVector3f_Sub(&center, &center, &grip);
    tsfp_matrix_set_position(target, center);
    return 1;
}

static inline int tsfp_weapon_cuff_target(const TsfpWeaponProfile *profile, XrVector3f wrist,
                                         const XrMatrix4x4f *forearm, unsigned hand, XrMatrix4x4f *target)
{
    XrVector3f toward_wrist;
    const XrVector3f elbow = tsfp_matrix_position(forearm);
    XrVector3f_Sub(&toward_wrist, &wrist, &elbow);
    if (!(XrVector3f_Length(&toward_wrist) > .05f))
        return 0;
    XrVector3f_Normalize(&toward_wrist);

    /* Cortez's mirrored forearm meshes have their outer surface on +/-Z.
     * Use the posed forearm so wrist flexion cannot roll the cuff. */
    const float side = hand == TSFP_AVATAR_RIGHT ? 1.f : -1.f;
    const XrVector3f outside = {side * forearm->m[8], side * forearm->m[9], side * forearm->m[10]};
    XrMatrix4x4f cuff_basis, arm_basis, from_cuff_basis;
    if (!tsfp_weapon_basis(&cuff_basis, profile->forward, profile->up) ||
        !tsfp_weapon_basis(&arm_basis, toward_wrist, outside))
        return 0;
    XrMatrix4x4f_InvertRigidBody(&from_cuff_basis, &cuff_basis);
    /* Allow room around the glove while keeping the housing short along the arm. */
    /* In the mount frame, turn -90 degrees around X and roll 180 around
     * the cuff's resulting long axis. Translate below to keep its opening fixed. */
    const XrMatrix4x4f turn = {{-1,0,0,0, 0,0,1,0, 0,1,0,0, 0,0,0,1}};
    XrMatrix4x4f turned_arm, scale, scaled_arm;
    XrMatrix4x4f_Multiply(&turned_arm, &arm_basis, &turn);
    XrMatrix4x4f_CreateScale(&scale, 1.35f, 1.35f, .70f);
    XrMatrix4x4f_Multiply(&scaled_arm, &turned_arm, &scale);
    XrMatrix4x4f_Multiply(target, &scaled_arm, &from_cuff_basis);

    /* In the forearm basis, +Y points outward and +Z points toward the wrist. */
    const XrVector3f placement = {0, .050f, -.165f};
    XrVector3f opening, offset, center;
    XrMatrix4x4f_TransformVector3f(&opening, target, &profile->grip);
    XrMatrix4x4f_TransformVector3f(&offset, &arm_basis, &placement);
    XrVector3f_Add(&center, &wrist, &offset);
    XrVector3f_Sub(&center, &center, &opening);
    tsfp_matrix_set_position(target, center);
    return 1;
}

static inline int tsfp_weapon_correction(const XrMatrix4x4f *source, const XrMatrix4x4f *target,
                                        XrMatrix4x4f *camera_move)
{
    const float *m = source->m;
    const XrVector3f x = {m[0],m[1],m[2]}, y = {m[4],m[5],m[6]}, z = {m[8],m[9],m[10]};
    XrVector3f cross;
    XrVector3f_Cross(&cross, &x, &y);
    if (!XrMatrix4x4f_IsAffine(source, 1e-4f) || !(fabsf(XrVector3f_Dot(&cross, &z)) > 1e-6f))
        return 0;
    XrMatrix4x4f from_source;
    XrMatrix4x4f_Invert(&from_source, source);
    XrMatrix4x4f_Multiply(camera_move, target, &from_source);
    for (unsigned i = 0; i < 16; ++i)
        if (!isfinite(camera_move->m[i]))
            return 0;
    return 1;
}

/* Preserve the Kruger placement verified in the second mission. */
static inline void tsfp_kruger_target(const XrMatrix4x4f *held_root, XrMatrix4x4f *target)
{
    XrMatrix4x4f offset;
    XrMatrix4x4f_CreateTranslation(&offset, 0, -.035f, -.181f);
    XrMatrix4x4f_Multiply(target, held_root, &offset);
}

static inline void tsfp_weapon_bore(const TsfpWeaponProfile *profile, const XrMatrix4x4f *target,
                                   XrVector3f *muzzle, XrVector3f *direction)
{
    XrMatrix4x4f_TransformVector3f(muzzle, target, &profile->muzzle);
    const XrVector3f origin = tsfp_matrix_position(target);
    XrMatrix4x4f_TransformVector3f(direction, target, &profile->forward);
    XrVector3f_Sub(direction, direction, &origin);
    XrVector3f_Normalize(direction);
}

static inline void tsfp_weapon_world_bore(const XrMatrix4x4f *world_from_camera,
                                         XrVector3f camera_muzzle, XrVector3f camera_direction,
                                         XrVector3f *muzzle, XrVector3f *direction)
{
    XrMatrix4x4f rotation = *world_from_camera;
    tsfp_matrix_set_position(&rotation, (XrVector3f){0});
    XrMatrix4x4f_TransformVector3f(muzzle, world_from_camera, &camera_muzzle);
    XrMatrix4x4f_TransformVector3f(direction, &rotation, &camera_direction);
    XrVector3f_Normalize(direction);
}

/* Keep the game's relative projectile and tracer directions around the bore. */
static inline void tsfp_weapon_aim_shot(XrVector3f ray, XrVector3f directions[3])
{
    const XrVector3f old_ray = directions[0];
    if (XrVector3f_Length(&old_ray) > .001f) {
        const XrQuaternionf rotation = tsfp_arm_rotation(old_ray, ray, (XrVector3f){0,1,0});
        for (unsigned i = 1; i < 3; ++i) {
            const XrVector3f direction = directions[i];
            XrQuaternionf_RotateVector3f(&directions[i], &rotation, &direction);
        }
    }
    directions[0] = ray;
}

#endif
