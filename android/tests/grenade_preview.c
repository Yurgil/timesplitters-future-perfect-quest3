#include "grenade_preview.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    float wall, back_wall, floor;
    unsigned calls;
    int invalid;
} Scene;

static int sweep(void *opaque, XrVector3f start, XrVector3f end, uint32_t *room, XrVector3f *hit)
{
    Scene *scene = opaque;
    ++scene->calls;
    assert(*room == 1u || *room == 2u);
    assert(start.x < 3.0f || *room == 2u);
    if (scene->invalid)
        return -1;
    float fraction = 2.0f;
    /* Try the farther plane first so the callback has to pick the nearest. */
    const float walls[] = {scene->back_wall, scene->wall};
    for (unsigned i = 0; i < sizeof walls / sizeof walls[0]; ++i)
        if (start.x < walls[i] && end.x >= walls[i])
            fraction = fminf(fraction, (walls[i] - start.x) / (end.x - start.x));
    if (start.y >= scene->floor && end.y < scene->floor)
        fraction = fminf(fraction, (start.y - scene->floor) / (start.y - end.y));
    if (fraction <= 1.0f) {
        XrVector3f_Lerp(hit, &start, &end, fraction);
        return 1;
    }
    /* Room tests use the walls sufficiently far from the mock portal. */
    if (start.x < 1.0f && end.x >= 1.11f)
        *room = 2;
    return 0;
}

static void near(float left, float right)
{
    assert(fabsf(left - right) < .0001f);
}

static void check_wrist_origin(void)
{
    const float half_turn = sqrtf(0.5f);
    const XrPosef anchor = {.orientation={0,half_turn,0,half_turn}, .position={5,1,-3}};
    const XrVector3f first = {-0.2f, -0.3f, -0.5f}, moved = {-0.4f, -0.2f, -0.7f};
    XrPosef grip = {.orientation=anchor.orientation};
    XrVector3f start, direction;
    XrPosef_TransformVector3f(&grip.position, &anchor, &first);
    tsfp_grenade_camera_ray(&anchor, &grip, &start, &direction);
    near(start.x, first.x); near(start.y, first.y); near(start.z, first.z);
    near(direction.x, 0); near(direction.y, 0); near(direction.z, -1);

    XrPosef_TransformVector3f(&grip.position, &anchor, &moved);
    tsfp_grenade_camera_ray(&anchor, &grip, &start, &direction);
    near(start.x, moved.x); near(start.y, moved.y); near(start.z, moved.z);

    TsfpGrenadeSource source = {.camera={0,2,0}, .room=1};
    Scene scene = {.wall=INFINITY, .back_wall=INFINITY, .floor=-INFINITY};
    uint32_t room = 0;
    start = (XrVector3f){1.5f,2,0};
    assert(tsfp_grenade_clear_origin(&source, 0, sweep, &scene, &start, &room));
    near(start.x, 1.5f);
    assert(scene.calls == 1 && room == 2);

    scene.wall = .5f;
    source.camera.y -= .07f;
    assert(tsfp_grenade_clear_origin(&source, .07f, sweep, &scene, &start, &room));
    near(start.x, .47f); near(start.y, 2);
    assert(scene.calls == 3 && room == 1);

    scene.invalid = 1;
    assert(!tsfp_grenade_clear_origin(&source, 0, sweep, &scene, &start, &room));
    near(start.x, .47f);
}

int main(void)
{
    check_wrist_origin();
    const struct {
        uint32_t weapon, kind;
        float launch_speed, charge, gravity, fuse, lookahead, probe_drop;
    } definitions[] = {
        {22, 12, .95f, .6f, -21.0f, 1.9f, .11f, 0},
        {23, 15, .95f, .6f, -27.0f, .75f, .11f, 0},
        {24, 14, .95f, 0, -27.0f, 2.4f, 0, 0},
        {34, 20, .60f, .6f, -10.0f, 10.0f, 0, .07f},
    };
    for (unsigned i = 0; i < sizeof definitions / sizeof definitions[0]; ++i) {
        const uint32_t weapon = definitions[i].weapon, kind = definitions[i].kind;
        TsfpGrenadeFlight flight = {.origin={0,2,0}, .direction={1,0,0}};
        assert(tsfp_grenade_flight(&flight, weapon, kind, definitions[i].launch_speed,
                                   definitions[i].charge));
        near(flight.speed, 60.0f * definitions[i].launch_speed);
        near(flight.gravity, definitions[i].gravity);
        near(flight.lifetime, definitions[i].fuse);
        near(flight.lookahead, definitions[i].lookahead);
        assert(flight.kind == kind && tsfp_grenade_weapon(weapon));
        near(flight.probe_drop, definitions[i].probe_drop);

        TsfpGrenadeArc arc;
        Scene walls = {.wall=10, .back_wall=10.5f, .floor=-1000};
        tsfp_grenade_trace(&flight, 1, sweep, &walls, &arc);
        assert(arc.contact && arc.count > 2 && walls.calls < 16);
        near(arc.points[arc.count - 1].x, walls.wall);
        const XrVector3f third_tick = arc.points[1];
        near(third_tick.x, definitions[i].launch_speed * (1 + .99f + .99f*.99f));
        near(third_tick.y, 2 + flight.gravity * (.99f + .99f*.99f + .99f) / 3600);

        flight.direction = (XrVector3f){0,0,-1};
        Scene floor = {.wall=INFINITY, .back_wall=INFINITY, .floor=0};
        tsfp_grenade_trace(&flight, 1, sweep, &floor, &arc);
        assert(arc.contact && arc.count > 2);
        near(arc.points[arc.count - 1].y, 0);

        Scene open = {.wall=INFINITY, .back_wall=INFINITY, .floor=-INFINITY};
        tsfp_grenade_trace(&flight, 1, sweep, &open, &arc);
        assert(!arc.contact);
        const unsigned ticks = (unsigned)ceilf(flight.lifetime * TSFP_GRENADE_TICKS);
        assert(arc.count == 1u + (ticks + TSFP_GRENADE_SWEEP_TICKS - 1) / TSFP_GRENADE_SWEEP_TICKS);
        assert(open.calls == arc.count - 1);

        open.invalid = 1;
        tsfp_grenade_trace(&flight, 1, sweep, &open, &arc);
        assert(!arc.contact && arc.count == 1);
    }
    TsfpGrenadeFlight invalid;
    assert(!tsfp_grenade_flight(&invalid, 24, 12, .95f, 0));
    assert(!tsfp_grenade_flight(&invalid, 22, 15, .95f, 0));
    assert(!tsfp_grenade_flight(&invalid, 22, 12, NAN, 0));
    puts("Grenade preview: four flight profiles, nearest wall/floor contact, rooms and lifetime passed");
    return 0;
}
