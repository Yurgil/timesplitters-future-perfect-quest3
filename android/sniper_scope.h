#ifndef TSFP_SNIPER_SCOPE_H
#define TSFP_SNIPER_SCOPE_H

#include "stereo_math.h"
#include "weapon_mount.h"

enum { TSFP_SCOPE_ZOOM_STEPS = 3 };

typedef struct TsfpScopeGeometry {
    XrVector3f center, muzzle, forward, right, up;
    float radius;
} TsfpScopeGeometry;

typedef struct {
    XrVector3f center;
    float radius;
} TsfpScopeProfile;

static inline const TsfpScopeProfile *tsfp_scope_profile(uint32_t id)
{
    /* From the Xbox housing meshes: above the bolt rifle's rear sight,
     * behind the Sniper Rifle's rear glass and the Future Sniper's optic. */
    static const TsfpScopeProfile bolt = {{-.270f, -.001f, .052f}, .031f};
    static const TsfpScopeProfile rifle = {{-.329f, -.0003f, .0598f}, .030f};
    static const TsfpScopeProfile future = {{.001f, .140f, .207f}, .038f};
    switch (id) {
    case TSFP_WEAPON_SNIPER_BOLT: return &bolt;
    case TSFP_WEAPON_SNIPER_RIFLE: return &rifle;
    case TSFP_WEAPON_FUTURE_SNIPER: return &future;
    default: return NULL;
    }
}

static inline TsfpScopeGeometry tsfp_scope_geometry(uint32_t id, const TsfpScopeProfile *scope,
                                                    int story_scene, const XrMatrix4x4f *mount)
{
    const TsfpWeaponProfile *weapon = tsfp_weapon_profile(id);
    TsfpScopeGeometry result = {.radius = scope->radius};
    tsfp_weapon_bore(weapon, mount, &result.muzzle, &result.forward);
    XrVector3f center = scope->center;
    /* The water-tower pickup reports Xbox weapon ID 8 with the
     * sniperrifle_anim housing. Its rear rim is at X = -0.3242 m. */
    if (id == TSFP_WEAPON_SNIPER_BOLT && story_scene == TSFP_SCENE_RUSSIAN_CONNECTION)
        center = (XrVector3f){-.334f, -.0003f, .0598f};
    if (id == TSFP_WEAPON_FUTURE_SNIPER && story_scene == TSFP_SCENE_TIME_TO_SPLIT)
        center.y += .005f;
    XrMatrix4x4f_TransformVector3f(&result.center, mount, &center);
    const XrVector3f origin = tsfp_matrix_position(mount);
    XrMatrix4x4f_TransformVector3f(&result.up, mount, &weapon->up);
    XrVector3f_Sub(&result.up, &result.up, &origin);
    XrVector3f_Cross(&result.right, &result.forward, &result.up);
    XrVector3f_Normalize(&result.right);
    XrVector3f_Cross(&result.up, &result.right, &result.forward);
    return result;
}

static inline XrView tsfp_scope_view(const TsfpScopeGeometry *scope, unsigned zoom, const XrPosef *anchor)
{
    /* A 6 cm lens at arm's length subtends about 6 degrees. The view must
     * be narrower than that to enlarge the scene even at the first step. */
    static const float half_angles[TSFP_SCOPE_ZOOM_STEPS] = {.032f, .019f, .011f};
    const float angle = half_angles[zoom];
    const XrVector3f r = scope->right, u = scope->up, f = scope->forward;
    const XrMatrix4x4f rotation = {{
        r.x, r.y, r.z, 0, u.x, u.y, u.z, 0, -f.x, -f.y, -f.z, 0, 0, 0, 0, 1
    }};
    XrPosef camera = {.position = scope->muzzle};
    XrMatrix4x4f_GetRotation(&camera.orientation, &rotation);
    XrView view = {.type = XR_TYPE_VIEW, .fov = {-angle, angle, angle, -angle}};
    XrPosef_Multiply(&view.pose, anchor, &camera);
    return view;
}

#endif
