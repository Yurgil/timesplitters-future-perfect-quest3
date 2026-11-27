#ifndef TSFP_GRENADE_PREVIEW_H
#define TSFP_GRENADE_PREVIEW_H

#include "xr_linear.h"

enum { TSFP_GRENADE_TICKS = 60, TSFP_GRENADE_SWEEP_TICKS = 3 };
enum { TSFP_GRENADE_PLASMA_STICKY_KIND = 14, TSFP_GRENADE_TIME_KIND = 20 };
#define TSFP_GRENADE_MAX_POINTS 201

typedef struct {
    XrVector3f origin, direction;
    float speed, gravity, lifetime, lookahead, probe_drop;
    uint32_t kind;
} TsfpGrenadeFlight;

typedef struct {
    XrVector3f camera;
    uint32_t shooter, room;
} TsfpGrenadeSource;

typedef struct {
    TsfpGrenadeFlight flight;
    XrMatrix4x4f view;
    TsfpGrenadeSource source;
} TsfpGrenadePreview;

typedef struct {
    XrVector3f points[TSFP_GRENADE_MAX_POINTS];
    unsigned count;
    int contact;
} TsfpGrenadeArc;

/* A sweep updates the room and returns 1 for contact, 0 for open space,
 * or -1 when there is no valid scene query. */
typedef int (*TsfpGrenadeSweep)(void *context, XrVector3f start, XrVector3f end,
                                uint32_t *room, XrVector3f *hit);

static inline int tsfp_grenade_vector_valid(XrVector3f v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* Touch's combined pose carries the grip position and aim orientation.
 * Convert both through the same unshifted camera space that the eyes use. */
static inline void tsfp_grenade_camera_ray(const XrPosef *anchor, const XrPosef *grip,
                                           XrVector3f *origin, XrVector3f *direction)
{
    XrPosef camera_from_local;
    XrPosef_Invert(&camera_from_local, anchor);
    XrPosef_TransformVector3f(origin, &camera_from_local, &grip->position);
    XrVector3f local_forward;
    XrQuaternionf_RotateVector3f(&local_forward, &grip->orientation, &(XrVector3f){0,0,-1});
    XrQuaternionf_RotateVector3f(direction, &camera_from_local.orientation, &local_forward);
}

static inline int tsfp_grenade_clear_origin(const TsfpGrenadeSource *source, float probe_drop,
                                            TsfpGrenadeSweep sweep, void *context,
                                            XrVector3f *origin, uint32_t *room)
{
    XrVector3f probe = *origin, hit;
    probe.y -= probe_drop;
    *room = source->room;
    const int result = sweep(context, source->camera, probe, room, &hit);
    if (result <= 0)
        return result == 0;
    if (!tsfp_grenade_vector_valid(hit))
        return 0;

    /* A tracked hand can cross solid geometry. Keep the launch just on
     * the camera side and resolve the room at that backed-off point. */
    XrVector3f ray, offset;
    XrVector3f_Sub(&ray, &probe, &source->camera);
    const float distance = XrVector3f_Length(&ray);
    if (!(distance > 1e-4f) || !isfinite(distance))
        return 0;
    XrVector3f_Sub(&offset, &hit, &source->camera);
    const float contact = XrVector3f_Dot(&offset, &ray) / distance;
    const float safe_distance = fminf(distance, fmaxf(0.0f, contact - 0.03f));
    XrVector3f_Scale(&offset, &ray, safe_distance / distance);
    XrVector3f_Add(&probe, &source->camera, &offset);
    *room = source->room;
    if (sweep(context, source->camera, probe, room, &hit) != 0)
        return 0;
    probe.y += probe_drop;
    *origin = probe;
    return 1;
}

static inline int tsfp_grenade_weapon(uint32_t id)
{
    return (id >= 22u && id <= 24u) || id == 34u;
}

static inline uint32_t tsfp_grenade_weapon_for_kind(uint32_t kind)
{
    switch (kind) {
    case 12u: return 22u;
    case 15u: return 23u;
    case TSFP_GRENADE_PLASMA_STICKY_KIND: return 24u;
    case TSFP_GRENADE_TIME_KIND: return 34u;
    default: return 0;
    }
}

static inline float tsfp_grenade_probe_drop(uint32_t kind)
{
    return kind == TSFP_GRENADE_TIME_KIND ? 0.07f : 0.0f;
}

static inline int tsfp_grenade_flight(TsfpGrenadeFlight *flight, uint32_t weapon, uint32_t kind,
                                      float speed_per_tick, float charge)
{
    if (tsfp_grenade_weapon_for_kind(kind) != weapon ||
        !isfinite(speed_per_tick) || speed_per_tick <= 0 || !isfinite(charge))
        return 0;
    flight->speed = speed_per_tick * TSFP_GRENADE_TICKS;
    if (!isfinite(flight->speed))
        return 0;
    flight->kind = kind;
    flight->lookahead = kind == 12u || kind == 15u ? 0.11f : 0.0f;
    flight->probe_drop = tsfp_grenade_probe_drop(kind);
    /* EAAF0 scales launch speed by the tick rate. F2B80 updates kinds 12
     * and 15, F39A0 handles sticky plasma (14) and F51F0 handles kind 20. */
    switch (weapon) {
    case 22:
        flight->gravity = -27.0f + 10.0f * charge;
        flight->lifetime = 1.0f + 1.5f * charge;
        return charge >= 0.0f && charge <= 1.0f;
    case 23:
        flight->gravity = -27.0f;
        flight->lifetime = 0.75f;
        return 1;
    case 24:
        flight->gravity = -27.0f;
        flight->lifetime = 2.4f;
        return 1;
    case 34:
        flight->gravity = -10.0f;
        flight->lifetime = 10.0f;
        return 1;
    default:
        return 0;
    }
}

static inline int tsfp_grenade_sweep(TsfpGrenadeSweep sweep, void *context,
                                     const TsfpGrenadeFlight *flight,
                                     XrVector3f start, XrVector3f end, XrVector3f velocity,
                                     uint32_t *room, XrVector3f *hit)
{
    start.y -= flight->probe_drop;
    end.y -= flight->probe_drop;
    const float speed = XrVector3f_Length(&velocity);
    if (speed > 1e-4f) {
        XrVector3f extra;
        XrVector3f_Scale(&extra, &velocity, flight->lookahead / speed);
        XrVector3f_Add(&end, &end, &extra);
    }
    return sweep(context, start, end, room, hit);
}

static inline void tsfp_grenade_trace(const TsfpGrenadeFlight *flight, uint32_t room,
                                      TsfpGrenadeSweep sweep, void *context, TsfpGrenadeArc *arc)
{
    arc->contact = 0;
    arc->count = 1;
    arc->points[0] = flight->origin;
    const float dt = 1.0f / TSFP_GRENADE_TICKS;
    const unsigned ticks = (unsigned)ceilf(flight->lifetime * TSFP_GRENADE_TICKS);
    XrVector3f velocity;
    XrVector3f_Scale(&velocity, &flight->direction, flight->speed);
    for (unsigned tick = 0; tick < ticks && arc->count < TSFP_GRENADE_MAX_POINTS;) {
        XrVector3f positions[TSFP_GRENADE_SWEEP_TICKS + 1] = {arc->points[arc->count - 1]};
        XrVector3f velocities[TSFP_GRENADE_SWEEP_TICKS];
        unsigned steps = 0;
        while (steps < TSFP_GRENADE_SWEEP_TICKS && tick < ticks) {
            velocities[steps] = velocity;
            XrVector3f delta;
            XrVector3f_Scale(&delta, &velocity, dt);
            XrVector3f_Add(&positions[steps + 1], &positions[steps], &delta);
            /* DC6F0 adds gravity, then the projectile update applies drag. */
            velocity.y += flight->gravity * dt;
            XrVector3f_Scale(&velocity, &velocity, 0.99f);
            ++steps;
            ++tick;
        }
        XrVector3f hit;
        uint32_t next_room = room;
        int result = tsfp_grenade_sweep(sweep, context, flight, positions[0], positions[steps],
                                        velocities[steps - 1], &next_room, &hit);
        if (result > 0 && steps > 1) {
            /* Refine the reported hit on the original game-tick segments. */
            next_room = room;
            for (unsigned i = 0; i < steps; ++i) {
                result = tsfp_grenade_sweep(sweep, context, flight, positions[i], positions[i + 1],
                                           velocities[i], &next_room, &hit);
                if (result)
                    break;
            }
        }
        if (result < 0 || (result > 0 && !tsfp_grenade_vector_valid(hit)))
            break;
        arc->points[arc->count++] = result > 0 ? hit : positions[steps];
        if (result > 0) {
            arc->contact = 1;
            break;
        }
        room = next_room;
    }
}

int tsfp_stereo_grenade_preview(TsfpGrenadePreview *preview);
int tsfp_stereo_grenade_launch(uint32_t shooter, TsfpGrenadeSource *source,
                               XrVector3f *origin, XrVector3f directions[3]);
void tsfp_grenade_render(const TsfpGrenadeArc *arc, const XrMatrix4x4f *view);
void tsfp_grenade_render_destroy(void);

#endif
