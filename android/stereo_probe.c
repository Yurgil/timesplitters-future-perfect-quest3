#include "stereo_probe.h"
#include "avatar_pose.h"
#include "stereo_backend.h"
#include "stereo_math.h"
#include "uplink_math.h"
#include "uplink_render.h"
#include "xr.h"

#include "guest_virtual_memory.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <string.h>

static int enabled;
static _Thread_local unsigned camera_calls;
static _Thread_local struct {
    uint32_t body, header, arms, view_weapon[TSFP_AVATAR_HANDS], weapon_id[TSFP_AVATAR_HANDS], flash_weapon;
    uint32_t shield[TSFP_AVATAR_HANDS];
    const uint8_t *body_nodes, *active_header, *palette;
    uint8_t *camera_palette;
    const void *world_projection, *active_projection;
    unsigned body_count, matrix_count, head, posed_hands, wrist_valid;
    int rig_supported, rig_sided, pose_prepared, palette_patched;
    XrVector3f body_offset, camera_room_offset;
    TsfpAvatarArm rig[TSFP_AVATAR_HANDS];
    float outward_x[TSFP_AVATAR_HANDS];
    XrMatrix4x4f animated_hand[TSFP_AVATAR_HANDS];
    XrMatrix4x4f posed[TSFP_AVATAR_HANDS][TSFP_ARM_MATRICES];
    XrMatrix4x4f saved[TSFP_AVATAR_HANDS][TSFP_ARM_MATRICES];
    XrVector3f wrist[TSFP_AVATAR_HANDS];
    XrQuaternionf wrist_orientation[TSFP_AVATAR_HANDS];
    XrQuaternionf camera_aim[TSFP_AVATAR_HANDS];
    TsfpModelRole role;
} avatar;
/* State that outlives tsfp_stereo_begin_frame. */
static _Thread_local struct {
    float body_back;
    uint32_t body, header;
    /* Camera-space hand bone with Touch aimed straight ahead. */
    XrMatrix4x4f hand_hold[TSFP_AVATAR_HANDS];
    unsigned hand_hold_valid;
    int hand_holds_loaded;
    /* The last camera-space move of each posed hand, for shots fired
     * between frames, and the hand that drew each actor hand slot. */
    XrMatrix4x4f hand_move[TSFP_AVATAR_HANDS];
    uint32_t hand_move_entity[TSFP_AVATAR_HANDS];
    unsigned hand_move_frame[TSFP_AVATAR_HANDS], slot_hand[TSFP_AVATAR_HANDS], frame;
    struct {
        XrVector3f muzzle, direction;
        uint32_t entity, weapon;
        unsigned frame;
    } shots[TSFP_AVATAR_HANDS];
    struct {
        uint32_t instance;
        XrMatrix4x4f source;
    } particle_weapons[TSFP_AVATAR_HANDS];
    struct {
        TsfpUplinkRay ray;
        XrVector3f emitters[2];
        uint32_t entity, preview_target;
        unsigned preview_frame;
        unsigned hand, frame;
    } uplink;
} kept = { .slot_hand = { TSFP_AVATAR_LEFT, TSFP_AVATAR_RIGHT } };

enum { TSFP_OFF_HAND_SLOT = TSFP_WEAPON_OFF_SLOT, TSFP_MAIN_HAND_SLOT = TSFP_WEAPON_MAIN_SLOT };
enum { TSFP_PLAYER_OFF_HAND = 0x184, TSFP_PLAYER_MAIN_HAND = 0x55c };
static const unsigned player_hand_state[] = { TSFP_PLAYER_OFF_HAND, TSFP_PLAYER_MAIN_HAND };
static _Thread_local struct {
    const void *active_projection;
    float projection[16], particle_projection[16], map_projection[16];
    XrMatrix4x4f particle_move, map_move;
} weapon;
static _Thread_local struct {
    uint32_t caller;
    float projection[16], view[16], position[3];
} camera;
/* Before a view renders, C8120 sets entity+0x2b4 bit 30 on the camera's
 * owner and on the weapons held by it, then clears it after the view.
 * A1AB0 skips every segment of an entity with this bit. */
enum { TSFP_ENTITY_PLANES = 0x2b4, TSFP_OWN_CAMERA = 0x40000000 };
static _Thread_local struct {
    uint32_t model[TSFP_AVATAR_HANDS];  /* by actor hand slot */
    unsigned drawn_slots, pistol_poses;
    XrMatrix4x4f correction[TSFP_AVATAR_HANDS];
    XrMatrix4x4f pistol_root[TSFP_AVATAR_HANDS];
} held;
static _Thread_local uint32_t object_cull_avatar[1 + TSFP_AVATAR_HANDS];
static const uint8_t *entity_from_handle(const tsfp_guest_vm *vm, uint32_t handle, uint32_t bytes);

void tsfp_stereo_probe_enable(int probe)
{
    enabled = probe;
    camera_calls = 0;
    camera.caller = 0;
    tsfp_stereo_begin_frame();
}

void tsfp_stereo_begin_frame(void)
{
    weapon.active_projection = NULL;
    ++kept.frame;
    memset(&avatar, 0, sizeof avatar);
    memset(&held, 0, sizeof held);
}

static uint32_t word(const void *src)
{
    uint32_t value;
    memcpy(&value, src, sizeof value);
    return value;
}

static uint32_t guest_word(const tsfp_guest_vm *vm, uint32_t address)
{
    const void *p = vm->resolve(vm->host, address, 4, 0);
    return p ? word(p) : 0;
}

static const uint8_t *local_player(const tsfp_guest_vm *vm, uint32_t address)
{
    const uint8_t *player = vm->resolve(vm->host, address, 0xe88u, 0);
    return player && word(player + 4) == 0 ? player : NULL;
}

static uint32_t render_model(const tsfp_guest_vm *vm, uint32_t entity)
{
    if (!entity)
        return 0;
    /* The drawable instance owns a back-pointer to the entity. The
     * structure at +0x24 shares its XBR header but starts with a type ID. */
    const uint32_t model = guest_word(vm, entity + 4u);
    return model && guest_word(vm, model) == entity ? model : 0;
}

static const uint8_t *model_camera_palette(const tsfp_guest_vm *vm, uint32_t instance, uint32_t view)
{
    if (!instance || view >= 4)
        return NULL;
    const uint32_t cache = guest_word(vm, instance + 0x20u + view * 4u);
    return cache && cache != guest_word(vm, instance + 0x10u) ?
        vm->resolve(vm->host, cache, sizeof(XrMatrix4x4f), 0) : NULL;
}

static void show_in_own_camera(const tsfp_guest_vm *vm, uint32_t entity)
{
    uint8_t *planes = vm->resolve(vm->host, entity + TSFP_ENTITY_PLANES, 4, 1);
    if (!planes)
        return;
    const uint32_t value = word(planes) & ~TSFP_OWN_CAMERA;
    memcpy(planes, &value, sizeof value);
}

/* 1BE870 stores the owning hand record at attachment+0x7c; that record
 * links back to the actor. */
static void show_held_weapons(const tsfp_guest_vm *vm, uint32_t body)
{
    const uint32_t actor = guest_word(vm, body + 0x7cu);
    if (!actor || guest_word(vm, actor) != body)
        return;
    for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot) {
        const uint32_t hand_record = actor + 0xa4u + slot * 0x130u;
        const uint32_t attachment = guest_word(vm, actor + 0x18cu + slot * 0x130u);
        held.model[slot] = 0;
        if (!attachment || guest_word(vm, attachment + 0x7cu) != hand_record ||
            guest_word(vm, hand_record + 0xecu) != actor)
            continue;
        show_in_own_camera(vm, attachment);
        held.model[slot] = render_model(vm, attachment);
    }
}

static void identify_viewmodels(const tsfp_guest_vm *vm, const uint8_t *player)
{
    /* The guest first-person pass pairs actor+0xa4 with player+0x184
     * and actor+0x1d4 with player+0x55c. */
    avatar.arms = player ? render_model(vm, word(player + 0xe84u)) : 0;
    avatar.view_weapon[TSFP_OFF_HAND_SLOT] = player ? render_model(vm, word(player + 0x1d8u)) : 0;
    avatar.view_weapon[TSFP_MAIN_HAND_SLOT] = player ? render_model(vm, word(player + 0x5b0u)) : 0;
    avatar.weapon_id[TSFP_OFF_HAND_SLOT] = player ? word(player + TSFP_PLAYER_OFF_HAND + 0x30u) : UINT32_MAX;
    avatar.weapon_id[TSFP_MAIN_HAND_SLOT] = player ? word(player + TSFP_PLAYER_MAIN_HAND + 0x30u) : UINT32_MAX;

    const int main_weapon = avatar.weapon_id[TSFP_MAIN_HAND_SLOT] != UINT32_MAX &&
        (avatar.view_weapon[TSFP_MAIN_HAND_SLOT] || held.model[TSFP_MAIN_HAND_SLOT]);
    for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot) {
        kept.slot_hand[slot] = tsfp_weapon_hand(slot, main_weapon);
        avatar.shield[slot] = 0;
        if (avatar.weapon_id[slot] == TSFP_WEAPON_FUTURE_SNIPER) {
            /* 1BE366 creates FSNIPER_SHIELD beside the viewmodel. */
            const uint32_t shield = word(player + player_hand_state[slot] + 0x58u);
            avatar.shield[slot] = render_model(vm, shield);
        }
    }
}

int tsfp_stereo_world_projection(float out[16])
{
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    /* Known US fields for active local gameplay, used by the PC adapters. */
    const uint32_t scene = guest_word(&vm, 0x0079094Cu);
    if (scene > 100u || guest_word(&vm, 0x00790950u) != 1u ||
        (guest_word(&vm, 0x007DE458u) & 0x20000000u))
        return 0;
    const uint32_t pause = guest_word(&vm, 0x00761C48u);
    if (pause && (guest_word(&vm, pause) & 1u))
        return 0;
    const uint32_t camera_address = guest_word(&vm, 0x007B0CC0u);
    const unsigned char *camera = vm.resolve(vm.host, camera_address, 0x208u, 0);
    if (!camera)
        return 0;
    const void *projection = vm.resolve(vm.host, word(camera + 0x204u), 16 * sizeof(float), 0);
    if (!projection)
        return 0;
    memcpy(out, projection, 16 * sizeof(float));
    return 1;
}

void *tsfp_stereo_begin_visibility(float saved[16], int objects)
{
    if (!tsfp_xr_avatar_ready())
        return NULL;
    float lens[16];
    if (!tsfp_stereo_world_projection(lens))
        return NULL;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t address = guest_word(&vm, 0x007B0CC0u);
    const uint8_t *camera = vm.resolve(vm.host, address, 0x218u, 0);
    const uint8_t *player = local_player(&vm, guest_word(&vm, 0x007B0C7Cu));
    if (!camera || !player || word(player + 8u) != address || word(camera + 0x214u) != 0)
        return NULL;
    const uint32_t mode = word(player + 0x114u);
    if (mode != 2u && mode != 4u && mode != 0x800u)
        return NULL;
    const float *view = vm.resolve(vm.host, word(camera + 0x208u), 16 * sizeof(float), 0);
    void *view_projection = vm.resolve(vm.host, word(camera + 0x20cu), 16 * sizeof(float), 1);
    float visibility[16];
    if (!view || !view_projection || !tsfp_xr_visibility_matrix(view, lens, visibility, objects))
        return NULL;
    if (objects) {
        const uint32_t entity = word(player + 0x14u);
        const uint32_t actor = entity ? guest_word(&vm, entity + 0x7cu) : 0;
        object_cull_avatar[0] = entity;
        for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot)
            object_cull_avatar[slot + 1] = actor && guest_word(&vm, actor) == entity ?
                guest_word(&vm, actor + 0x18cu + slot * 0x130u) : 0;
    }
    memcpy(saved, view_projection, sizeof visibility);
    memcpy(view_projection, visibility, sizeof visibility);
    return view_projection;
}

int tsfp_stereo_object_outside(uint32_t entity, const float minimum[3], const float maximum[3])
{
    /* The guest bounds precede IK and cannot describe the local avatar. */
    for (unsigned i = 0; i < 1 + TSFP_AVATAR_HANDS; ++i)
        if (entity == object_cull_avatar[i])
            return 0;
    return tsfp_xr_object_outside(minimum, maximum);
}

void tsfp_stereo_probe_weapon(uint32_t address)
{
    if (!tsfp_xr_geometry_enabled())
        return;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const void *lens = vm.resolve(vm.host, address, sizeof weapon.projection, 0);
    /* The Xbox projection setter copies the active lens into this matrix.
     * Comparing the whole matrix scopes the change to the viewmodel pass. */
    weapon.active_projection = lens ?
        vm.resolve(vm.host, 0x0055FA40u, sizeof weapon.projection, 0) : NULL;
    if (weapon.active_projection) {
        memcpy(weapon.projection, lens, sizeof weapon.projection);
        if (avatar.role == TSFP_MODEL_UPLINK_MAP)
            memcpy(weapon.map_projection, lens, sizeof weapon.map_projection);
        /* A spawn or weapon change can replace a viewmodel after the world pass. */
        const uint8_t *player = local_player(&vm, guest_word(&vm, 0x007B0C7Cu));
        identify_viewmodels(&vm, player);
    }
}

const float *tsfp_stereo_weapon_projection(void)
{
    if (avatar.role == TSFP_MODEL_LEFT_WEAPON_PARTICLES || avatar.role == TSFP_MODEL_RIGHT_WEAPON_PARTICLES)
        return weapon.particle_projection;
    if (avatar.role == TSFP_MODEL_UPLINK_MAP)
        return weapon.map_projection;
    return weapon.active_projection &&
        memcmp(weapon.active_projection, weapon.projection, sizeof weapon.projection) == 0 ?
        weapon.projection : NULL;
}

int tsfp_stereo_weapon_depth_range(float *offset, float *scale)
{
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    /* sub_00023150 sends these viewport constants to NV2A c-38/c-37.
     * The HLE divides the resulting Z by the full D24 range. */
    const uint8_t *viewport = vm.resolve(vm.host, 0x00563058u, 32u, 0);
    if (!viewport)
        return 0;
    memcpy(scale, viewport + 8, sizeof *scale);
    memcpy(offset, viewport + 24, sizeof *offset);
    *scale /= 16777215.0f;
    *offset /= 16777215.0f;
    return isfinite(*offset) && *scale >= 1e-6f && *scale <= 1.0f;
}

static int identify_body_rig(const tsfp_guest_vm *vm)
{
    avatar.header = word(avatar.active_header);
    if (kept.body != avatar.body || kept.header != avatar.header) {
        kept.body = avatar.body;
        kept.header = avatar.header;
        memset(kept.hand_move_entity, 0, sizeof kept.hand_move_entity);
        memset(kept.shots, 0, sizeof kept.shots);
        memset(kept.particle_weapons, 0, sizeof kept.particle_weapons);
        kept.uplink.entity = 0;
    }
    avatar.body_count = guest_word(vm, avatar.header);
    avatar.head = 0;
    avatar.palette = NULL;
    avatar.rig_supported = avatar.rig_sided = 0;
    avatar.pose_prepared = avatar.posed_hands = avatar.wrist_valid = 0;
    if (!avatar.body_count || avatar.body_count > TSFP_XBR_MAX_AVATAR_NODES)
        return 0;
    const size_t bytes = avatar.body_count * TSFP_XBR_NODE_BYTES;
    if (avatar.header < bytes)
        return 0;
    avatar.body_nodes = vm->resolve(vm->host, avatar.header - bytes, bytes, 0);
    if (!avatar.body_nodes)
        return 0;
    const unsigned chest = tsfp_avatar_spine_end(avatar.body_nodes, avatar.body_count);
    if (!chest)
        return 0;
    avatar.matrix_count = guest_word(vm, avatar.header + 4u);
    avatar.head = tsfp_avatar_arm_rig(avatar.body_nodes, avatar.body_count, chest,
                                      avatar.matrix_count, avatar.rig);
    avatar.rig_supported = avatar.head != 0;
    if (!avatar.head) {
        /* Keep the native torso visible for an unfamiliar arm skeleton. */
        unsigned branches[3];
        if (tsfp_avatar_bone_children(avatar.body_nodes, avatar.body_count, chest, branches) != 3)
            return 0;
        avatar.head = branches[0];
    }
    return 1;
}

int tsfp_stereo_avatar_after_cull(uint32_t entity)
{
    if (!tsfp_xr_avatar_ready() || !entity)
        return 0;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t player_addr = guest_word(&vm, 0x007B0C7Cu);
    const uint8_t *player = local_player(&vm, player_addr);
    if (!player || word(player + 0x14u) != entity || word(player + 0x114u) != 2)
        return 0;
    const uint32_t camera = word(player + 8);
    if (!camera || camera != guest_word(&vm, 0x007B0CC0u))
        return 0;
    const uint8_t *view = vm.resolve(vm.host, camera, 0x218u, 0);
    uint8_t *body = vm.resolve(vm.host, entity, 0x154u, 1);
    if (!view || !body || word(body + 0x20u) != 8 || word(body + 0x150u) != player_addr)
        return 0;
    const uint32_t flags = word(body + 0x28u);
    if (flags & 0x80000000u)
        return 0;
    const uint32_t view_index = word(view + 0x214u);
    if (view_index >= 4)
        return 0;
    const uint32_t body_model = render_model(&vm, entity);
    const uint8_t *header = body_model ? vm.resolve(vm.host, body_model + 4u, 4, 0) : NULL;
    const void *world_lens = vm.resolve(vm.host, word(view + 0x204u), 16 * sizeof(float), 0);
    const void *active_lens = vm.resolve(vm.host, 0x0055FA40u, 16 * sizeof(float), 0);
    const uint32_t viewport = guest_word(&vm, 0x007E71C8u);
    const uint8_t *rect = vm.resolve(vm.host, viewport, 0x1cu, 0);
    if (!header || !world_lens || !active_lens || !rect ||
        memcmp(world_lens, active_lens, 16 * sizeof(float)) != 0)
        return 0;

    avatar.body = body_model;
    avatar.active_header = header;
    avatar.world_projection = world_lens;
    avatar.active_projection = active_lens;
    if ((!avatar.head || word(avatar.active_header) != avatar.header) && !identify_body_rig(&vm)) {
        avatar.body = 0;
        return 0;
    }

    /* A2430 clips against the Xbox camera and its portal rectangle.
     * Reuse its native draw bucket but let the actual stereo eyes clip
     * the local body. The cached Xbox room and transform mode stay intact. */
    const float bounds[4] = { (int32_t)word(rect), (int32_t)word(rect + 0x14u),
        (int32_t)word(rect + 4u), (int32_t)word(rect + 0x18u) };
    if (!(bounds[0] < bounds[2] && bounds[1] < bounds[3]))
        return 0;
    memcpy(body + 0x118u, bounds, sizeof bounds);
    const uint32_t visible = word(body + 0x0cu) | (1u << view_index);
    const uint32_t drawable = (flags & ~0x00802000u) | 0x00400000u;
    memcpy(body + 0x0cu, &visible, sizeof visible);
    memcpy(body + 0x28u, &drawable, sizeof drawable);
    const uint32_t planes = guest_word(&vm, entity + TSFP_ENTITY_PLANES);
    show_in_own_camera(&vm, entity);
    show_held_weapons(&vm, entity);
    identify_viewmodels(&vm, player);
    static int reported;
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[6DoF] Local body in native world pass, view %u flags %08x to %08x, "
                        "planes %08x, held slots %08x %08x\n",
                view_index, flags, drawable, planes,
                held.model[0], held.model[1]);
    }
    return 1;
}

static int avatar_world_pass(void)
{
    return avatar.world_projection && avatar.active_projection &&
        memcmp(avatar.active_projection, avatar.world_projection, 16 * sizeof(float)) == 0;
}

static void report_pose_failure(unsigned reason)
{
    static const char *const reasons[] = { "the arm rig is unsupported", "the model palette is missing",
        "the camera palette is missing", "the palette basis is not rigid" };
    static unsigned reported;
    if (!(reported & (1u << reason))) {
        reported |= 1u << reason;
        fprintf(stderr, "[6DoF] Body pose skipped: %s\n", reasons[reason]);
    }
}

static XrVector3f palette_position(const uint8_t *palette, unsigned matrix)
{
    XrMatrix4x4f bone;
    memcpy(&bone, palette + matrix * sizeof bone, sizeof bone);
    return tsfp_matrix_position(&bone);
}

/* Game camera space has +X to the right and +Z behind the view, as the
 * OpenXR anchor it is drawn in. */
static void orient_body_rig(void)
{
    const unsigned left = avatar.rig[TSFP_AVATAR_LEFT].matrix[TSFP_ARM_UPPER];
    const unsigned right = avatar.rig[TSFP_AVATAR_RIGHT].matrix[TSFP_ARM_UPPER];
    const int swap = palette_position(avatar.camera_palette, left).x >
        palette_position(avatar.camera_palette, right).x;
    if (swap) {
        const TsfpAvatarArm arm = avatar.rig[TSFP_AVATAR_LEFT];
        avatar.rig[TSFP_AVATAR_LEFT] = avatar.rig[TSFP_AVATAR_RIGHT];
        avatar.rig[TSFP_AVATAR_RIGHT] = arm;
    }
    const float model_left = palette_position(avatar.palette, avatar.rig[TSFP_AVATAR_LEFT].matrix[TSFP_ARM_UPPER]).x;
    const float model_right = palette_position(avatar.palette, avatar.rig[TSFP_AVATAR_RIGHT].matrix[TSFP_ARM_UPPER]).x;
    avatar.outward_x[TSFP_AVATAR_RIGHT] = model_right > model_left ? 1.0f : -1.0f;
    avatar.outward_x[TSFP_AVATAR_LEFT] = -avatar.outward_x[TSFP_AVATAR_RIGHT];
    static int reported = -1;
    if (reported != swap) {
        reported = swap;
        fprintf(stderr, "[6DoF] Character arms %s node order, right shoulder on model %cX\n",
                swap ? "swapped from" : "match", avatar.outward_x[TSFP_AVATAR_RIGHT] > 0 ? '+' : '-');
    }
}

/* The game camera sits at the character's eyes, which leaves the view just
 * above the neck. Keep the shoulders behind the eyes and ease the shift so
 * animation bob does not slide the feet. */
static void push_body_back(const XrPosef *anchor)
{
    enum { SHOULDERS_BEHIND_EYES_MM = 120, MAX_PUSH_MM = 450 };
    const XrVector3f left = palette_position(avatar.camera_palette,
                                             avatar.rig[TSFP_AVATAR_LEFT].matrix[TSFP_ARM_UPPER]);
    const XrVector3f right = palette_position(avatar.camera_palette,
                                              avatar.rig[TSFP_AVATAR_RIGHT].matrix[TSFP_ARM_UPPER]);
    const float shoulders = (left.z + right.z) * 0.5f;
    const float wanted = fminf(MAX_PUSH_MM * 1e-3f,
                               fmaxf(0.0f, SHOULDERS_BEHIND_EYES_MM * 1e-3f - shoulders));
    if (isfinite(wanted))
        kept.body_back += (wanted - kept.body_back) * (kept.body_back ? 0.1f : 1.0f);
    const XrVector3f back = { 0, 0, kept.body_back };
    XrVector3f shift;
    XrQuaternionf_RotateVector3f(&shift, &anchor->orientation, &back);
    XrVector3f_Add(&avatar.body_offset, &avatar.body_offset, &shift);
    static int reported;
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[6DoF] Shoulders %.3f m behind the eyes, body moved back %.3f m\n",
                shoulders, kept.body_back);
    }
}

static XrMatrix4x4f linear_part(const XrMatrix4x4f *matrix)
{
    XrMatrix4x4f linear = *matrix;
    tsfp_matrix_set_position(&linear, (XrVector3f){0});
    return linear;
}

static const char *const hand_names[TSFP_AVATAR_HANDS] = { "left", "right" };

static void load_hand_holds(void)
{
    kept.hand_holds_loaded = 1;
    const char *path = tsfp_xr_hand_calibration_file();
    FILE *file = path && *path ? fopen(path, "r") : NULL;
    if (!file)
        return;
    char name[8];
    float m[9];
    while (fscanf(file, "%7s %f %f %f %f %f %f %f %f %f", name,
                  &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7], &m[8]) == 10) {
        for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand) {
            if (strcmp(name, hand_names[hand]) != 0)
                continue;
            XrMatrix4x4f *hold = &kept.hand_hold[hand];
            XrMatrix4x4f_CreateIdentity(hold);
            for (unsigned column = 0; column < 3; ++column)
                memcpy(&hold->m[column * 4], &m[column * 3], 3 * sizeof(float));
            kept.hand_hold_valid |= 1u << hand;
        }
    }
    fclose(file);
    fprintf(stderr, "[6DoF] Loaded hand calibration (left %d, right %d)\n",
            !!(kept.hand_hold_valid & 1u), !!(kept.hand_hold_valid & 2u));
}

/* The player turns Touch until a hand looks right. That look becomes the
 * one for Touch aimed straight ahead. */
static void calibrate_hand_holds(void)
{
    for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand) {
        if (!(avatar.posed_hands & (1u << hand)))
            continue;
        kept.hand_hold[hand] = linear_part(&avatar.posed[hand][TSFP_ARM_HAND]);
        kept.hand_hold_valid |= 1u << hand;
    }
    const char *path = tsfp_xr_hand_calibration_file();
    FILE *file = path && *path ? fopen(path, "w") : NULL;
    for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand) {
        if (!(kept.hand_hold_valid & (1u << hand)))
            continue;
        const float *m = kept.hand_hold[hand].m;
        char line[256];
        snprintf(line, sizeof line, "%s %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f\n", hand_names[hand],
                 m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
        if (file)
            fputs(line, file);
        fprintf(stderr, "[6DoF] Hand calibration %s", line);
    }
    if (file)
        fclose(file);
}

static void move_held_weapon(unsigned hand, unsigned source_hand)
{
    if (!(avatar.posed_hands & (1u << hand)))
        return;
    XrMatrix4x4f hand_from_camera;
    XrMatrix4x4f_Invert(&hand_from_camera, &avatar.animated_hand[source_hand]);
    XrMatrix4x4f_Multiply(&held.correction[hand], &avatar.posed[hand][TSFP_ARM_HAND], &hand_from_camera);
    kept.hand_move[hand] = held.correction[hand];
    kept.hand_move[hand].m[12] += avatar.camera_room_offset.x;
    kept.hand_move[hand].m[13] += avatar.camera_room_offset.y;
    kept.hand_move[hand].m[14] += avatar.camera_room_offset.z;
    kept.hand_move_frame[hand] = kept.frame;
}

static void prepare_avatar_pose(uint32_t view)
{
    XrPosef anchor, hands[TSFP_AVATAR_HANDS];
    const unsigned tracked = tsfp_xr_avatar_poses(&anchor, &avatar.body_offset, hands);
    if (!avatar.rig_supported || view >= 4) {
        report_pose_failure(0);
        return;
    }

    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    avatar.palette = vm.resolve(vm.host, guest_word(&vm, avatar.body + 0x10u),
                                avatar.matrix_count * sizeof(XrMatrix4x4f), 0);
    if (!avatar.palette) {
        report_pose_failure(1);
        return;
    }
    /* sub_A1B80 builds the view's composed palette before sub_18EA90
     * draws the first node. The renderer reads this cache, not model+0x10. */
    const uint32_t cache = guest_word(&vm, avatar.body + 0x20u + view * 4u);
    avatar.camera_palette = vm.resolve(vm.host, cache, avatar.matrix_count * sizeof(XrMatrix4x4f), 1);
    if (!avatar.camera_palette || avatar.camera_palette == avatar.palette) {
        report_pose_failure(2);
        return;
    }
    if (!avatar.rig_sided) {
        orient_body_rig();
        avatar.rig_sided = 1;
    }

    const unsigned shoulder = avatar.rig[TSFP_AVATAR_RIGHT].matrix[TSFP_ARM_UPPER];
    XrMatrix4x4f model_shoulder, camera_shoulder, camera_from_body;
    memcpy(&model_shoulder, avatar.palette + shoulder * sizeof model_shoulder, sizeof model_shoulder);
    memcpy(&camera_shoulder, avatar.camera_palette + shoulder * sizeof camera_shoulder, sizeof camera_shoulder);
    if (!tsfp_avatar_palette_basis(&camera_from_body, &model_shoulder, &camera_shoulder)) {
        report_pose_failure(3);
        return;
    }
    static int reported_origin;
    if (!reported_origin) {
        reported_origin = 1;
        const XrVector3f root = tsfp_matrix_position(&camera_from_body);
        fprintf(stderr, "[6DoF] Body origin in game camera space %.3f %.3f %.3f\n",
                root.x, root.y, root.z);
    }

    push_body_back(&anchor);
    XrMatrix4x4f local_from_camera, local_from_body, body_from_local;
    XrVector3f_Add(&anchor.position, &anchor.position, &avatar.body_offset);
    XrMatrix4x4f_CreateFromRigidTransform(&local_from_camera, &anchor);
    XrMatrix4x4f_Multiply(&local_from_body, &local_from_camera, &camera_from_body);
    XrMatrix4x4f_InvertRigidBody(&body_from_local, &local_from_body);
    XrQuaternionf camera_from_local;
    XrQuaternionf_Invert(&camera_from_local, &anchor.orientation);
    XrQuaternionf_RotateVector3f(&avatar.camera_room_offset, &camera_from_local, &avatar.body_offset);
    const uint32_t entity = guest_word(&vm, avatar.body);
    const XrMatrix4x4f camera_from_body_linear = linear_part(&camera_from_body);
    XrMatrix4x4f body_from_camera_linear;
    XrMatrix4x4f_InvertRigidBody(&body_from_camera_linear, &camera_from_body_linear);
    if (!kept.hand_holds_loaded)
        load_hand_holds();

    for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand) {
        const unsigned bit = 1u << hand;
        XrMatrix4x4f *bones = avatar.posed[hand];
        for (unsigned i = 0; i < TSFP_ARM_MATRICES; ++i) {
            const unsigned matrix = tsfp_avatar_pose_matrix(&avatar.rig[hand], i);
            memcpy(&bones[i], avatar.palette + matrix * sizeof bones[i], sizeof bones[i]);
        }
        memcpy(&avatar.animated_hand[hand],
               avatar.camera_palette + avatar.rig[hand].matrix[TSFP_ARM_HAND] * sizeof(XrMatrix4x4f),
               sizeof(XrMatrix4x4f));
        avatar.wrist_orientation[hand] = anchor.orientation;
        if (tracked & bit) {
            XrQuaternionf aim_from_camera;
            XrQuaternionf_Multiply(&aim_from_camera, &hands[hand].orientation, &camera_from_local);
            avatar.camera_aim[hand] = aim_from_camera;
            XrMatrix4x4f aim;
            XrMatrix4x4f_CreateFromQuaternion(&aim, &aim_from_camera);
            /* An uncalibrated hand keeps its animated look for Touch held
             * straight ahead. */
            const XrMatrix4x4f animated = linear_part(&bones[TSFP_ARM_HAND]);
            XrMatrix4x4f hold, hand_camera, hand_body, animated_inverse, wrist_rotation;
            if (kept.hand_hold_valid & bit)
                hold = kept.hand_hold[hand];
            else
                XrMatrix4x4f_Multiply(&hold, &camera_from_body_linear, &animated);
            XrMatrix4x4f_Multiply(&hand_camera, &aim, &hold);
            XrMatrix4x4f_Multiply(&hand_body, &body_from_camera_linear, &hand_camera);
            XrMatrix4x4f_Invert(&animated_inverse, &animated);
            XrMatrix4x4f_Multiply(&wrist_rotation, &hand_body, &animated_inverse);
            XrVector3f target;
            XrMatrix4x4f_TransformVector3f(&target, &body_from_local, &hands[hand].position);
            if (tsfp_avatar_pose_arm(bones, avatar.outward_x[hand], target, &wrist_rotation)) {
                avatar.posed_hands |= bit;
                avatar.wrist_orientation[hand] = hands[hand].orientation;
            }
        }
        const XrVector3f wrist = tsfp_matrix_position(&bones[TSFP_ARM_WRIST_JOINT]);
        XrMatrix4x4f_TransformVector3f(&avatar.wrist[hand], &local_from_body, &wrist);
        if (isfinite(avatar.wrist[hand].x) && isfinite(avatar.wrist[hand].y) && isfinite(avatar.wrist[hand].z))
            avatar.wrist_valid |= bit;
        if (avatar.posed_hands & bit) {
            for (unsigned i = 0; i < TSFP_ARM_MATRICES; ++i) {
                XrMatrix4x4f posed;
                XrMatrix4x4f_Multiply(&posed, &camera_from_body, &bones[i]);
                bones[i] = posed;
            }
            move_held_weapon(hand, hand);
            kept.hand_move_entity[hand] = entity;
        }
    }
    if (tsfp_xr_take_hand_calibration())
        calibrate_hand_holds();
    static _Thread_local uint32_t reported_header;
    static _Thread_local unsigned reported_mask, reports;
    if (reports < 24u && tracked && (reported_header != avatar.header || reported_mask != avatar.posed_hands)) {
        reported_header = avatar.header;
        reported_mask = avatar.posed_hands;
        ++reports;
        fprintf(stderr, "[6DoF] Body rig %08x (%u nodes, %u matrices), controller mask %u, posed mask %u\n",
                avatar.header, avatar.body_count, avatar.matrix_count, tracked, avatar.posed_hands);
    }
}

static void exchange_avatar_palette(int apply)
{
    for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand) {
        if (!(avatar.posed_hands & (1u << hand)))
            continue;
        for (unsigned i = 0; i < TSFP_ARM_MATRICES; ++i) {
            const unsigned matrix = avatar.rig[hand].matrix[i];
            if (matrix == TSFP_XBR_NO_NODE)
                continue;
            const size_t size = sizeof(XrMatrix4x4f);
            uint8_t *entry = avatar.camera_palette + matrix * size;
            if (apply)
                memcpy(&avatar.saved[hand][i], entry, size);
            memcpy(entry, apply ? &avatar.posed[hand][i] : &avatar.saved[hand][i], size);
        }
    }
    avatar.palette_patched = apply;
}

int tsfp_stereo_body_matrices(float rows[32])
{
    return tsfp_xr_body_matrices(avatar.body_offset, rows);
}

int tsfp_stereo_held_matrices(unsigned hand, float rows[32])
{
    if (hand >= TSFP_AVATAR_HANDS)
        return 0;
    XrMatrix4x4f identity;
    XrMatrix4x4f_CreateIdentity(&identity);
    const XrMatrix4x4f *correction = avatar.posed_hands & (1u << hand) ? &held.correction[hand] : &identity;
    return tsfp_xr_held_matrices(avatar.body_offset, correction->m, rows);
}

static int view_arms_supported(const tsfp_guest_vm *vm)
{
    const uint32_t header = avatar.arms ? guest_word(vm, avatar.arms + 4u) : 0;
    /* The Xbox first-person arms share this 41-node, 24-matrix rig. */
    return header && guest_word(vm, header) == 41u && guest_word(vm, header + 4u) == 24u;
}

static void locate_uplink_emitters(const tsfp_guest_vm *vm, uint32_t cache, unsigned matrices,
                                   unsigned hand, XrVector3f wrist, const XrMatrix4x4f *camera_move)
{
    /* The ends of the two articulated emitter chains are matrices 10 and 16. */
    static const unsigned tip_matrix[] = {10, 16};
    XrVector3f forward, up, right;
    const XrQuaternionf aim = avatar.camera_aim[hand];
    XrQuaternionf_RotateVector3f(&forward, &aim, &(XrVector3f){0,0,-1});
    XrQuaternionf_RotateVector3f(&up, &aim, &(XrVector3f){0,-1,0});
    XrVector3f_Cross(&right, &forward, &up);
    XrVector3f_Normalize(&forward);
    XrVector3f_Normalize(&up);
    XrVector3f_Normalize(&right);
    const uint8_t *palette = matrices > tip_matrix[1] ?
        vm->resolve(vm->host, cache, (tip_matrix[1] + 1) * sizeof(XrMatrix4x4f), 0) : NULL;
    for (unsigned i = 0; i < 2; ++i) {
        XrVector3f tip;
        if (palette) {
            const XrVector3f original = palette_position(palette, tip_matrix[i]);
            XrMatrix4x4f_TransformVector3f(&tip, camera_move, &original);
        }
        XrVector3f offset;
        if (palette)
            XrVector3f_Sub(&offset, &tip, &wrist);
        if (!palette || !(XrVector3f_Length(&offset) < .65f)) {
            XrVector3f_Scale(&offset, &right, i ? .042f : -.042f);
            XrVector3f_Add(&tip, &wrist, &offset);
            XrVector3f_Scale(&offset, &up, .038f);
            XrVector3f_Add(&tip, &tip, &offset);
        }
        XrVector3f_Add(&kept.uplink.emitters[i], &tip, &avatar.camera_room_offset);
    }
    XrVector3f_Lerp(&kept.uplink.ray.origin, &kept.uplink.emitters[0], &kept.uplink.emitters[1], .5f);
    kept.uplink.ray.forward = forward;
    kept.uplink.ray.up = up;
    kept.uplink.entity = kept.hand_move_entity[hand];
    kept.uplink.hand = hand;
    kept.uplink.frame = kept.frame;
}

static XrVector3f posed_wrist(unsigned hand)
{
    return tsfp_matrix_position(&avatar.posed[hand][TSFP_ARM_WRIST_JOINT]);
}

static int weapon_target(uint32_t id, unsigned hand, XrMatrix4x4f *target)
{
    if (id == TSFP_WEAPON_KRUGER && (held.pistol_poses & (1u << hand))) {
        tsfp_kruger_target(&held.pistol_root[hand], target);
        return 1;
    }
    const TsfpWeaponProfile *profile = tsfp_weapon_profile(id);
    const XrVector3f wrist = posed_wrist(hand);
    if (profile->mount == TSFP_WEAPON_CUFF)
        return tsfp_weapon_cuff_target(profile, wrist, &avatar.posed[hand][TSFP_ARM_FOREARM], hand, target);
    return tsfp_weapon_target(id, hand, wrist, avatar.camera_aim[hand], target);
}

static int weapon_mount(const tsfp_guest_vm *vm, unsigned slot, unsigned hand,
                        XrMatrix4x4f *camera_move, XrMatrix4x4f *mounted_pose)
{
    const uint32_t id = avatar.weapon_id[slot];
    const TsfpWeaponProfile *profile = tsfp_weapon_profile(id);
    if (!profile)
        return 0;
    const uint32_t instance = avatar.view_weapon[slot];
    const uint32_t header = instance ? guest_word(vm, instance + 4u) : 0;
    const unsigned nodes = header ? guest_word(vm, header) : 0;
    const unsigned matrices = header ? guest_word(vm, header + 4u) : 0;
    if (profile->node >= nodes || nodes > TSFP_XBR_MAX_AVATAR_NODES ||
        header < nodes * TSFP_XBR_NODE_BYTES)
        return 0;
    const uint8_t *node = vm->resolve(vm->host,
        header - (nodes - profile->node) * TSFP_XBR_NODE_BYTES, TSFP_XBR_NODE_BYTES, 0);
    if (!node || node[TSFP_XBR_MATRIX] >= matrices)
        return 0;
    const uint32_t cache = guest_word(vm, instance + 0x20u);
    const void *housing = cache ?
        vm->resolve(vm->host, cache + node[TSFP_XBR_MATRIX] * sizeof(XrMatrix4x4f),
                    sizeof(XrMatrix4x4f), 0) : NULL;
    if (!housing)
        return 0;

    XrMatrix4x4f target, source;
    if (!weapon_target(id, hand, &target))
        return 0;
    memcpy(&source, housing, sizeof source);
    if (!tsfp_weapon_correction(&source, &target, camera_move))
        return 0;
    if (mounted_pose)
        *mounted_pose = target;

    kept.particle_weapons[slot].instance = instance;
    kept.particle_weapons[slot].source = source;
    if (profile->mount == TSFP_WEAPON_GUN) {
        XrVector3f muzzle, direction;
        tsfp_weapon_bore(profile, &target, &muzzle, &direction);
        XrVector3f_Add(&kept.shots[slot].muzzle, &muzzle, &avatar.camera_room_offset);
        kept.shots[slot].direction = direction;
        kept.shots[slot].weapon = id;
        kept.shots[slot].entity = kept.hand_move_entity[hand];
        kept.shots[slot].frame = kept.frame;
    } else if (id == TSFP_UPLINK) {
        locate_uplink_emitters(vm, cache, matrices, hand, posed_wrist(hand), camera_move);
    }
    static uint64_t reported;
    const uint64_t bit = UINT64_C(1) << id;
    if (!(reported & bit)) {
        reported |= bit;
        fprintf(stderr, "[6DoF] Mounted weapon %u (%s), node %u, matrix %u, %s hand\n",
                id, profile->asset, profile->node, node[TSFP_XBR_MATRIX], hand_names[hand]);
    }
    return 1;
}

int tsfp_stereo_weapon_attachment(TsfpModelRole role, XrVector3f *body_offset, XrMatrix4x4f *camera_move)
{
    const unsigned hand = role == TSFP_MODEL_LEFT_WEAPON ? TSFP_AVATAR_LEFT : TSFP_AVATAR_RIGHT;
    if (!(avatar.posed_hands & (1u << hand)))
        return 0;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const unsigned slot = kept.slot_hand[TSFP_MAIN_HAND_SLOT] == hand && avatar.view_weapon[TSFP_MAIN_HAND_SLOT] &&
        avatar.weapon_id[TSFP_MAIN_HAND_SLOT] != UINT32_MAX ? TSFP_MAIN_HAND_SLOT : TSFP_OFF_HAND_SLOT;
    if (weapon_mount(&vm, slot, hand, camera_move, NULL)) {
        for (unsigned i = 0; i < 16; ++i)
            if (!isfinite(camera_move->m[i]))
                return 0;
        *body_offset = avatar.body_offset;
        return 1;
    }
    if (!view_arms_supported(&vm))
        return 0;
    const uint32_t cache = guest_word(&vm, avatar.arms + 0x20u);
    const uint8_t *palette = cache ? vm.resolve(vm.host, cache, 15 * sizeof(XrMatrix4x4f), 0) : NULL;
    if (!palette)
        return 0;
    /* Both model variants have their own origin. The original palm that
     * holds the detail mesh is the common reference for every weapon. */
    const XrVector3f first_shoulder = palette_position(palette, 1u);
    const XrVector3f second_shoulder = palette_position(palette, 10u);
    const unsigned first_hand = first_shoulder.x > second_shoulder.x ? TSFP_AVATAR_RIGHT : TSFP_AVATAR_LEFT;
    const unsigned palm_index = hand == first_hand ? 5u : 14u;
    XrMatrix4x4f source_palm, from_source_camera;
    memcpy(&source_palm, palette + palm_index * sizeof source_palm, sizeof source_palm);
    XrMatrix4x4f_Invert(&from_source_camera, &source_palm);
    XrMatrix4x4f_Multiply(camera_move, &avatar.posed[hand][TSFP_ARM_HAND], &from_source_camera);
    for (unsigned i = 0; i < 16; ++i)
        if (!isfinite(camera_move->m[i]))
            return 0;
    *body_offset = avatar.body_offset;
    static unsigned logged;
    if (!(logged & (1u << hand))) {
        logged |= 1u << hand;
        fprintf(stderr, "[6DoF] Aligned first-person weapon to %s palm (source matrix %u)\n",
                hand_names[hand], palm_index);
    }
    return 1;
}

int tsfp_stereo_weapon_pose(TsfpModelRole role, XrPosef *pose)
{
    const unsigned hand = role == TSFP_MODEL_LEFT_WEAPON || role == TSFP_MODEL_LEFT_FLASH ?
        TSFP_AVATAR_LEFT : TSFP_AVATAR_RIGHT;
    const unsigned bit = 1u << hand;
    uint32_t gun = avatar.flash_weapon;
    if (!gun)
        for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot)
            if (kept.slot_hand[slot] == hand && avatar.view_weapon[slot])
                gun = avatar.view_weapon[slot];
    if (!gun)
        return 0;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    /* View 0's composed viewmodel root is already relative to its camera.
     * Use the original grip offset when the first draw has no cached root. */
    const uint32_t root = guest_word(&vm, gun + 0x20u);
    const void *origin = root ? vm.resolve(vm.host, root + 0x30u, sizeof(XrVector3f), 0) : NULL;
    XrVector3f camera_wrist = { hand == TSFP_AVATAR_LEFT ? -0.050f : 0.050f, -0.035f, -0.105f };
    if (origin) {
        XrVector3f cached;
        memcpy(&cached, origin, sizeof cached);
        if (cached.z < -0.01f && XrVector3f_Length(&cached) < 2.0f)
            camera_wrist = cached;
    }
    XrPosef anchor, hands[TSFP_AVATAR_HANDS];
    XrVector3f body_offset;
    const unsigned tracked = tsfp_xr_avatar_poses(&anchor, &body_offset, hands);
    const int from_body = (avatar.wrist_valid & bit) != 0;
    if (!from_body && !(tracked & bit))
        return 0;
    const XrVector3f wrist = from_body ? avatar.wrist[hand] : hands[hand].position;
    XrQuaternionf orientation = from_body ? avatar.wrist_orientation[hand] : hands[hand].orientation;
    if ((role == TSFP_MODEL_LEFT_WEAPON || role == TSFP_MODEL_RIGHT_WEAPON) && (tracked & bit)) {
        /* Touch Plus aim and the viewmodel use opposite up axes. Turn
         * around the local aim ray to keep the bore pointing forward. */
        const XrQuaternionf roll = { .z = 1.0f };
        XrQuaternionf upright;
        XrQuaternionf_Multiply(&upright, &roll, &orientation);
        orientation = upright;
    }
    *pose = tsfp_wrist_pose(wrist, orientation, camera_wrist);
    static unsigned calls;
    if (calls++ % 600u == 0 && (tracked & bit))
        fprintf(stderr, "[6DoF] %s weapon from %s at %.3f %.3f %.3f, grip %.3f %.3f %.3f, "
                        "source wrist %.3f %.3f %.3f\n",
                hand == TSFP_AVATAR_LEFT ? "left" : "right", from_body ? "body wrist" : "controller",
                pose->position.x, pose->position.y, pose->position.z,
                hands[hand].position.x, hands[hand].position.y, hands[hand].position.z,
                camera_wrist.x, camera_wrist.y, camera_wrist.z);
    return 1;
}

enum {
    TSFP_HAND_RECORDS = 0xa4,
    TSFP_HAND_RECORD_SIZE = 0x130,
    TSFP_CAMERA_VIEW = 0x208,
    TSFP_SHOT_POSE_FRAMES = 4,
};

static const uint8_t *first_person_player(const tsfp_guest_vm *vm)
{
    const uint8_t *player = local_player(vm, guest_word(vm, 0x007B0C7Cu));
    return player && word(player + 0x114u) == 2 ? player : NULL;
}

int tsfp_stereo_immersive_player(void)
{
    if (!tsfp_xr_world_active())
        return 0;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    return first_person_player(&vm) != NULL;
}

int tsfp_stereo_sniper_shield_active(void)
{
    if (!tsfp_xr_world_active())
        return 0;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint8_t *player = first_person_player(&vm);
    if (!player)
        return 0;
    for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot) {
        const uint8_t *state = player + player_hand_state[slot];
        if (word(state + 0x30u) == TSFP_WEAPON_FUTURE_SNIPER &&
            (guest_word(&vm, word(state + 0x58u) + 0x28u) & 1u))
            return 1;
    }
    return 0;
}

int tsfp_stereo_hide_reticle(void)
{
    if (!tsfp_stereo_immersive_player())
        return 0;
    static int reported;
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[6DoF] Suppressed the camera-bound reticle\n");
    }
    return 1;
}

static int actor_hand_slot(const tsfp_guest_vm *vm, uint32_t entity, uint32_t record)
{
    const uint32_t actor = guest_word(vm, entity + 0x7cu);
    if (actor && guest_word(vm, actor) == entity)
        for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot)
            if (record == actor + TSFP_HAND_RECORDS + slot * TSFP_HAND_RECORD_SIZE)
                return (int)slot;
    return -1;
}

static int current_weapon_bore(unsigned slot, uint32_t entity, uint32_t weapon_id)
{
    return entity && kept.shots[slot].entity == entity && kept.shots[slot].weapon == weapon_id &&
        kept.frame - kept.shots[slot].frame <= TSFP_SHOT_POSE_FRAMES &&
        tsfp_xr_touch_tracked(kept.slot_hand[slot]);
}

int tsfp_stereo_weapon_effect_attachment(TsfpModelRole role, XrVector3f *body_offset,
                                         XrMatrix4x4f *camera_move)
{
    if (role == TSFP_MODEL_UPLINK_MAP) {
        *body_offset = avatar.body_offset;
        *camera_move = weapon.map_move;
        return 1;
    }
    if (role == TSFP_MODEL_LEFT_WEAPON_PARTICLES || role == TSFP_MODEL_RIGHT_WEAPON_PARTICLES) {
        *body_offset = avatar.body_offset;
        *camera_move = weapon.particle_move;
        return 1;
    }
    const unsigned hand = role == TSFP_MODEL_LEFT_SHIELD ? TSFP_AVATAR_LEFT : TSFP_AVATAR_RIGHT;
    const unsigned slot = kept.slot_hand[TSFP_MAIN_HAND_SLOT] == hand && avatar.shield[TSFP_MAIN_HAND_SLOT] ?
        TSFP_MAIN_HAND_SLOT : TSFP_OFF_HAND_SLOT;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint8_t *palette = model_camera_palette(&vm, avatar.shield[slot], 0);
    XrMatrix4x4f source, target;
    if (!palette || !weapon_target(TSFP_WEAPON_FUTURE_SNIPER, hand, &target))
        return 0;
    /* FSNIPER_SHIELD is authored in the rifle's coordinates. Its two
     * identity bones are composed into the camera palette by A1B80. */
    memcpy(&source, palette, sizeof source);
    if (!tsfp_weapon_correction(&source, &target, camera_move))
        return 0;
    *body_offset = avatar.body_offset;
    return 1;
}

int tsfp_stereo_prepare_uplink_map(uint32_t state)
{
    if (!avatar.posed_hands || !tsfp_xr_world_active())
        return 0;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint8_t *player = first_person_player(&vm);
    const uint32_t prop = guest_word(&vm, state + 0x54u);
    if (!player || !prop)
        return 0;
    identify_viewmodels(&vm, player);
    for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot) {
        const uint32_t id = avatar.weapon_id[slot];
        const unsigned hand = kept.slot_hand[slot];
        if ((id == TSFP_UPLINK || id == TSFP_UPLINK_MAP) &&
            prop == word(player + player_hand_state[slot] + 0x54u) &&
            (avatar.posed_hands & avatar.wrist_valid & (1u << hand))) {
            /* 34CAA0 emits its textured quads in the same view space as the cuff. */
            XrMatrix4x4f cuff_move, cuff_pose;
            if (!weapon_mount(&vm, slot, hand, &cuff_move, &cuff_pose))
                return 0;
            XrVector3f pivot;
            XrMatrix4x4f_TransformVector3f(&pivot, &cuff_pose, &tsfp_weapon_profile(id)->grip);
            const XrVector3f elbow = tsfp_matrix_position(&avatar.posed[hand][TSFP_ARM_FOREARM]);
            tsfp_uplink_rotate_map(&cuff_move, pivot, elbow, posed_wrist(hand), &weapon.map_move);
            static int reported;
            if (!reported) {
                reported = 1;
                fprintf(stderr, "[6DoF] Drawing the Uplink map from the tracked cuff\n");
            }
            return 1;
        }
    }
    return 0;
}

TsfpModelRole tsfp_stereo_weapon_particles(uint32_t emitter)
{
    if (!avatar.posed_hands || !tsfp_xr_world_active())
        return TSFP_MODEL_DEFAULT;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint8_t *effect = vm.resolve(vm.host, emitter, 0x8cu, 0);
    if (!effect)
        return TSFP_MODEL_DEFAULT;
    /* 112120 draws this flag's particles with an identity view matrix. */
    const int in_camera = (word(effect + 4u) & 0x10u) != 0;
    const uint32_t owner = word(effect + 0x88u);
    if (!in_camera && !owner)
        return TSFP_MODEL_DEFAULT;
    const uint8_t *player = first_person_player(&vm);
    if (!player)
        return TSFP_MODEL_DEFAULT;
    const uint32_t entity = word(player + 0x14u);
    if (!in_camera && !avatar.shield[TSFP_MAIN_HAND_SLOT] && !avatar.shield[TSFP_OFF_HAND_SLOT]) {
        int has_shield = 0;
        for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot) {
            const unsigned state = player_hand_state[slot];
            if (word(player + state + 0x30u) == TSFP_WEAPON_FUTURE_SNIPER && word(player + state + 0x38u) == 1u) {
                has_shield = 1;
                break;
            }
        }
        if (!has_shield)
            return TSFP_MODEL_DEFAULT;
    }
    int matched_slot = -1;
    const void *matched_source = NULL;
    int matched_held = 0;
    if (in_camera && !owner) {
        /* 000DFEC0 creates one-shot camera-space muzzle flashes without an owner pointer. */
        XrVector3f emitter_pos;
        memcpy(&emitter_pos, effect + 0x14u, sizeof emitter_pos);
        float best_dist = 1e30f;
        for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot) {
            const unsigned hand = kept.slot_hand[slot];
            const unsigned state = player_hand_state[slot];
            const uint32_t id = word(player + state + 0x30u);
            const uint32_t view_weapon = word(player + state + 0x54u);
            const uint32_t instance = view_weapon ? render_model(&vm, view_weapon) : 0;
            if (id == UINT32_MAX || !(avatar.posed_hands & (1u << hand)) ||
                !current_weapon_bore(slot, entity, id) || !instance ||
                instance != kept.particle_weapons[slot].instance)
                continue;
            const XrVector3f origin = tsfp_matrix_position(&kept.particle_weapons[slot].source);
            XrVector3f delta;
            XrVector3f_Sub(&delta, &emitter_pos, &origin);
            const float dist = XrVector3f_Length(&delta);
            if (matched_slot < 0 || dist < best_dist) {
                best_dist = dist;
                matched_slot = (int)slot;
                matched_source = &kept.particle_weapons[slot].source;
            }
        }
    } else {
        for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot) {
            const unsigned hand = kept.slot_hand[slot];
            const unsigned state = player_hand_state[slot];
            const uint32_t id = word(player + state + 0x30u);
            const int shield_active = id == TSFP_WEAPON_FUTURE_SNIPER && word(player + state + 0x38u) == 1u;
            if (id == UINT32_MAX || !(avatar.posed_hands & (1u << hand)) || !current_weapon_bore(slot, entity, id))
                continue;
            const void *source = NULL;
            if (in_camera && owner == word(player + state + 0x54u)) {
                const uint32_t instance = render_model(&vm, owner);
                if (instance && instance == kept.particle_weapons[slot].instance)
                    source = &kept.particle_weapons[slot].source;
            } else if (shield_active && owner == word(player + state + 0x58u)) {
                const uint32_t shield_instance = avatar.shield[slot] ? avatar.shield[slot] : render_model(&vm, owner);
                source = model_camera_palette(&vm, shield_instance, 0);
                if (!source && kept.particle_weapons[slot].instance)
                    source = &kept.particle_weapons[slot].source;
            }
            /* The shield toggle also enables effects on the actor's separately
             * attached rifle. Those particles arrive with the normal world view. */
            const int held_shield = !in_camera && shield_active && held.model[slot] &&
                guest_word(&vm, held.model[slot]) == owner;
            if (source || held_shield) {
                matched_slot = (int)slot;
                matched_source = source;
                matched_held = held_shield;
                break;
            }
        }
    }
    if (matched_slot >= 0) {
        const unsigned slot = (unsigned)matched_slot;
        const unsigned hand = kept.slot_hand[slot];
        const unsigned state = player_hand_state[slot];
        const uint32_t id = word(player + state + 0x30u);
        const int shield_active = id == TSFP_WEAPON_FUTURE_SNIPER && word(player + state + 0x38u) == 1u;
        const void *projection = vm.resolve(vm.host, 0x0055FA40u, sizeof weapon.particle_projection, 0);
        if (!projection)
            return TSFP_MODEL_DEFAULT;
        if (matched_held) {
            weapon.particle_move = held.correction[hand];
        } else {
            XrMatrix4x4f target, original;
            memcpy(&original, matched_source, sizeof original);
            if (!weapon_target(id, hand, &target) ||
                !tsfp_weapon_correction(&original, &target, &weapon.particle_move))
                return TSFP_MODEL_DEFAULT;
        }
        memcpy(weapon.particle_projection, projection, sizeof weapon.particle_projection);
        static int reported_shield;
        if (shield_active && !reported_shield) {
            reported_shield = 1;
            fprintf(stderr, "[6DoF] Drawing the Future Sniper's shield particles from the tracked rifle\n");
        }
        return hand == TSFP_AVATAR_LEFT ? TSFP_MODEL_LEFT_WEAPON_PARTICLES : TSFP_MODEL_RIGHT_WEAPON_PARTICLES;
    }
    return TSFP_MODEL_DEFAULT;
}

TsfpModelRole tsfp_stereo_set_model_role(TsfpModelRole role)
{
    const TsfpModelRole previous = avatar.role;
    avatar.role = role;
    return previous;
}

static int read_camera_view(const tsfp_guest_vm *vm, const uint8_t *player, XrMatrix4x4f *view)
{
    const uint32_t camera = word(player + 8u);
    const uint32_t matrix = camera ? guest_word(vm, camera + TSFP_CAMERA_VIEW) : 0;
    const void *source = matrix ? vm->resolve(vm->host, matrix, sizeof *view, 0) : NULL;
    if (!source)
        return 0;
    memcpy(view, source, sizeof *view);
    return 1;
}

static int current_uplink(const uint8_t *player)
{
    if (!player || !kept.uplink.entity || word(player + 0x14u) != kept.uplink.entity ||
        kept.frame - kept.uplink.frame > TSFP_SHOT_POSE_FRAMES || !tsfp_xr_touch_tracked(kept.uplink.hand))
        return 0;
    for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot)
        if (kept.slot_hand[slot] == kept.uplink.hand &&
            word(player + player_hand_state[slot] + 0x30u) == TSFP_UPLINK &&
            word(player + player_hand_state[slot] + 0x38u) == 1u)
            return 1;
    return 0;
}

static int uplink_world_ray(const tsfp_guest_vm *vm, const uint8_t *player, TsfpUplinkRay *ray)
{
    XrMatrix4x4f view, inverse;
    if (!current_uplink(player) || !read_camera_view(vm, player, &view))
        return 0;
    XrMatrix4x4f_Invert(&inverse, &view);
    tsfp_weapon_world_bore(&inverse, kept.uplink.ray.origin, kept.uplink.ray.forward,
                           &ray->origin, &ray->forward);
    tsfp_matrix_set_position(&inverse, (XrVector3f){0});
    XrMatrix4x4f_TransformVector3f(&ray->up, &inverse, &kept.uplink.ray.up);
    return isfinite(ray->origin.x) && isfinite(ray->origin.y) && isfinite(ray->origin.z) &&
        XrVector3f_Length(&ray->forward) > .99f && XrVector3f_Length(&ray->up) > .99f;
}

static _Thread_local struct {
    void *clip, *origin, *delta;
    XrMatrix4x4f saved_clip;
    XrVector3f saved_origin, saved_delta;
} uplink_scope;

typedef struct {
    void *pose, *reticle;
    XrVector3f saved_pose[2];
    float saved_reticle[2];
} TsfpUplinkPlayerPose;

static _Thread_local TsfpUplinkPlayerPose uplink_grab_pose, uplink_physics_pose;

static int begin_uplink_player_pose(const tsfp_guest_vm *vm, uint32_t player, const TsfpUplinkRay *ray,
                                    TsfpUplinkPlayerPose *scope)
{
    void *pose = vm->resolve(vm->host, player + 0xb4u, sizeof scope->saved_pose, 1);
    void *reticle = vm->resolve(vm->host, player + 0xf6cu, sizeof scope->saved_reticle, 1);
    if (!pose || !reticle)
        return 0;
    scope->pose = pose;
    scope->reticle = reticle;
    memcpy(scope->saved_pose, pose, sizeof scope->saved_pose);
    memcpy(scope->saved_reticle, reticle, sizeof scope->saved_reticle);
    const XrVector3f tracked_pose[] = {ray->origin, ray->forward};
    memcpy(pose, tracked_pose, sizeof tracked_pose);
    memset(reticle, 0, sizeof scope->saved_reticle);
    return 1;
}

static void end_uplink_player_pose(TsfpUplinkPlayerPose *scope)
{
    memcpy(scope->pose, scope->saved_pose, sizeof scope->saved_pose);
    memcpy(scope->reticle, scope->saved_reticle, sizeof scope->saved_reticle);
    scope->pose = NULL;
}

/* 3508B0 uses 25B910/25B7B0 to select and keep the target. Those helpers
 * project object centers with camera+20C, and the obstruction filter uses
 * camera+27C and +334. Restore them before any other guest pass runs. */
int tsfp_stereo_uplink_begin(uint32_t state)
{
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t player_address = guest_word(&vm, 0x007B0C7Cu);
    const uint8_t *player = first_person_player(&vm);
    if (!player || !tsfp_xr_world_active() || uplink_scope.clip ||
        (state != player_address + TSFP_PLAYER_OFF_HAND && state != player_address + TSFP_PLAYER_MAIN_HAND) ||
        guest_word(&vm, state + 0x30u) != TSFP_UPLINK)
        return 0;
    TsfpUplinkRay ray;
    const uint32_t camera = word(player + 8u);
    if (!camera || camera != guest_word(&vm, 0x007B0CC0u) || !uplink_world_ray(&vm, player, &ray))
        return 0;
    XrMatrix4x4f lens, clip;
    if (!tsfp_stereo_world_projection(lens.m) || !tsfp_uplink_view(&ray, &lens, &clip))
        return 0;
    void *projection = vm.resolve(vm.host, guest_word(&vm, camera + 0x20cu), sizeof clip, 1);
    void *origin = vm.resolve(vm.host, camera + 0x27cu, sizeof ray.origin, 1);
    void *delta = vm.resolve(vm.host, camera + 0x334u, sizeof ray.origin, 1);
    if (!projection || !origin || !delta ||
        !begin_uplink_player_pose(&vm, player_address, &ray, &uplink_grab_pose))
        return 0;
    const XrVector3f travel = tsfp_uplink_ray_delta(&ray, tsfp_uplink_range);
    uplink_scope.clip = projection;
    uplink_scope.origin = origin;
    uplink_scope.delta = delta;
    memcpy(&uplink_scope.saved_clip, projection, sizeof clip);
    memcpy(&uplink_scope.saved_origin, origin, sizeof ray.origin);
    memcpy(&uplink_scope.saved_delta, delta, sizeof travel);
    memcpy(projection, &clip, sizeof clip);
    memcpy(origin, &ray.origin, sizeof ray.origin);
    memcpy(delta, &travel, sizeof travel);
    return 1;
}

void tsfp_stereo_uplink_end(void)
{
    memcpy(uplink_scope.clip, &uplink_scope.saved_clip, sizeof uplink_scope.saved_clip);
    memcpy(uplink_scope.origin, &uplink_scope.saved_origin, sizeof uplink_scope.saved_origin);
    memcpy(uplink_scope.delta, &uplink_scope.saved_delta, sizeof uplink_scope.saved_delta);
    uplink_scope.clip = NULL;
    end_uplink_player_pose(&uplink_grab_pose);
}

static _Thread_local struct {
    void *selection, *filters;
    uint32_t saved_selection[4], saved_filters[5];
} uplink_preview;

uint32_t tsfp_stereo_uplink_preview_begin(void)
{
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t address = guest_word(&vm, 0x007B0C7Cu);
    const uint8_t *player = first_person_player(&vm);
    if (!current_uplink(player) || (kept.uplink.preview_frame &&
        kept.frame - kept.uplink.preview_frame < 3u))
        return 0;
    kept.uplink.preview_frame = kept.frame;
    kept.uplink.preview_target = 0;
    const uint32_t lock = guest_word(&vm, address + 0x1550u);
    if (lock && tsfp_xr_uplink_trigger(kept.uplink.hand))
        return 0;
    void *selection = vm.resolve(vm.host, address + 0x154cu, sizeof uplink_preview.saved_selection, 1);
    void *filters = vm.resolve(vm.host, 0x0078B540u, sizeof uplink_preview.saved_filters, 1);
    const unsigned slot = word(player + TSFP_PLAYER_MAIN_HAND + 0x30u) == TSFP_UPLINK ?
        TSFP_MAIN_HAND_SLOT : TSFP_OFF_HAND_SLOT;
    if (!selection || !filters || !tsfp_stereo_uplink_begin(address + player_hand_state[slot]))
        return 0;
    uplink_preview.selection = selection;
    uplink_preview.filters = filters;
    memcpy(uplink_preview.saved_selection, selection, sizeof uplink_preview.saved_selection);
    memcpy(uplink_preview.saved_filters, filters, sizeof uplink_preview.saved_filters);
    return address;
}

uint32_t tsfp_stereo_uplink_preview_target(uint32_t player)
{
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t handle = guest_word(&vm, player + 0x1550u);
    uint32_t address;
    return entity_from_handle(&vm, handle, 0x30u) &&
        tsfp_uplink_entity_address(guest_word(&vm, 0x0072F5E8u), handle, &address) ? address : 0;
}

void tsfp_stereo_uplink_preview_end(uint32_t target)
{
    memcpy(uplink_preview.selection, uplink_preview.saved_selection, sizeof uplink_preview.saved_selection);
    memcpy(uplink_preview.filters, uplink_preview.saved_filters, sizeof uplink_preview.saved_filters);
    tsfp_stereo_uplink_end();
    kept.uplink.preview_target = target;
}

int tsfp_stereo_uplink_physics_begin(uint32_t target_address)
{
    if (!tsfp_xr_world_active() || uplink_physics_pose.pose)
        return 0;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t player_address = guest_word(&vm, 0x007B0C7Cu);
    const uint8_t *player = first_person_player(&vm);
    const uint32_t owner = guest_word(&vm, target_address + 0x2e4u);
    const uint8_t *entity = entity_from_handle(&vm, owner, 0x154u);
    TsfpUplinkRay ray;
    if (!entity || word(entity + 0x150u) != player_address ||
        !uplink_world_ray(&vm, player, &ray))
        return 0;
    /* The callbacks use the player's pose for the distance clamp, spring
     * and launch impulse. Restore it outside the callbacks, including after
     * release, so the character and camera keep their own transforms. */
    if (!begin_uplink_player_pose(&vm, player_address, &ray, &uplink_physics_pose))
        return 0;
    static unsigned reported;
    const uint32_t callback = guest_word(&vm, target_address + 0x16cu);
    const unsigned bit = callback == 0x0034F5B0u ? 1u : 2u;
    if (!(reported & bit)) {
        reported |= bit;
        fprintf(stderr, "[6DoF] Uplink uses the tracked wrist for entity type %#x, callback %#x\n",
                guest_word(&vm, target_address + 0x20u), callback);
    }
    return 1;
}

void tsfp_stereo_uplink_physics_end(void)
{
    end_uplink_player_pose(&uplink_physics_pose);
}

int tsfp_stereo_uplink_visual(TsfpUplinkVisual *visual)
{
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t player_address = guest_word(&vm, 0x007B0C7Cu);
    const uint8_t *player = first_person_player(&vm);
    if (!current_uplink(player))
        return 0;
    const uint32_t lock = guest_word(&vm, player_address + 0x1550u);
    const uint32_t handle = lock ? lock : kept.uplink.preview_target;
    const uint8_t *target = entity_from_handle(&vm, handle, 0x1b4u);
    *visual = (TsfpUplinkVisual){
        .start = kept.uplink.ray.origin,
        .marker = tsfp_uplink_point(&kept.uplink.ray, 4.0f)
    };
    XrMatrix4x4f view;
    if (target && read_camera_view(&vm, player, &view)) {
        XrVector3f origin, offset;
        memcpy(&origin, target + 0x64u, sizeof origin);
        memcpy(&offset, target + 0x1a8u, sizeof offset);
        XrVector3f_Add(&origin, &origin, &offset);
        XrMatrix4x4f_TransformVector3f(&visual->marker, &view, &origin);
        visual->target = 1;
        visual->held = lock != 0;
        if (visual->held) {
            // The grab and entity updates own the lock. The effect follows its
            // target even when the original beam record is being recreated.
            visual->end = visual->marker;
            visual->age = 0.5f;
            visual->beam = 1;
        }
        static uint32_t logged_lock;
        if (lock && logged_lock != lock) {
            logged_lock = lock;
            fprintf(stderr, "[6DoF] Uplink locked %#x type %#x, asset %#x\n",
                    lock, word(target + 0x20u), word(target + 0x94u));
        }
    }
    const uint32_t beam_address = guest_word(&vm, player_address + 0x1558u);
    const uint8_t *beam = beam_address == 0x007789A0u &&
        guest_word(&vm, 0x00767680u) ? vm.resolve(vm.host, beam_address, 0x3cu, 0) : NULL;
    if (beam && word(beam + 4u) == word(player + 4u) &&
        read_camera_view(&vm, player, &view)) {
        float age, fade;
        memcpy(&age, beam + 0x34u, sizeof age);
        memcpy(&fade, beam + 0x38u, sizeof fade);
        if (!visual->held) {
            XrVector3f end;
            memcpy(&end, beam + 0x18u, sizeof end);
            XrMatrix4x4f_TransformVector3f(&visual->end, &view, &end);
            visual->beam = age > 0 && fade < .5f;
        }
        if (age > 0 && fade < .5f && isfinite(age) && isfinite(fade)) {
            visual->age = age;
            visual->fade = fade;
        }
    }
    return isfinite(visual->marker.x) && isfinite(visual->marker.y) && isfinite(visual->marker.z);
}

void tsfp_stereo_flash_position(uint32_t record, uint32_t output, int shot_effect)
{
    if (!tsfp_xr_world_active())
        return;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t player_address = guest_word(&vm, 0x007B0C7Cu);
    const uint8_t *player = first_person_player(&vm);
    if (!player)
        return;
    const uint32_t entity = word(player + 0x14u);
    int slot = entity ? actor_hand_slot(&vm, entity, record) : -1;
    if (shot_effect && record == player_address + TSFP_PLAYER_MAIN_HAND)
        slot = TSFP_MAIN_HAND_SLOT;
    else if (shot_effect && record == player_address + TSFP_PLAYER_OFF_HAND)
        slot = TSFP_OFF_HAND_SLOT;
    if (slot < 0)
        return;
    const unsigned state = player_hand_state[slot];
    const uint32_t id = word(player + state + 0x30u);
    const int has_weapon_particles = tsfp_weapon_profile(id) != NULL;
    if (!(has_weapon_particles || (!shot_effect && tsfp_stereo_weapon_projection())) ||
        !current_weapon_bore((unsigned)slot, entity, id))
        return;
    XrMatrix4x4f view, inverse;
    if (!read_camera_view(&vm, player, &view))
        return;
    void *position = vm.resolve(vm.host, output, sizeof(XrVector3f), 1);
    if (!position)
        return;
    XrMatrix4x4f_Invert(&inverse, &view);
    XrVector3f muzzle, direction;
    tsfp_weapon_world_bore(&inverse, kept.shots[slot].muzzle, kept.shots[slot].direction,
                           &muzzle, &direction);
    memcpy(position, &muzzle, sizeof muzzle);
}

/* 1C0200 calls 1B7680 with six pointers to its own position and direction
 * vectors. These copies feed the collision ray, projectile and tracer. */
void tsfp_stereo_redirect_projectile(uint32_t stack, uint32_t hand_record)
{
    if (!tsfp_xr_touch_tracked(TSFP_AVATAR_LEFT) && !tsfp_xr_touch_tracked(TSFP_AVATAR_RIGHT))
        return;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t player_address = guest_word(&vm, 0x007B0C7Cu);
    const uint8_t *player = first_person_player(&vm);
    const uint8_t *args = vm.resolve(vm.host, stack, 14u * sizeof(uint32_t), 0);
    if (!player || !args)
        return;
    const uint32_t entity = word(player + 0x14u);
    if (!entity || word(args + 4u) != entity)
        return;
    const uint32_t weapon_record = word(args + 8u);
    int slot = actor_hand_slot(&vm, entity, hand_record);
    if (weapon_record == player_address + TSFP_PLAYER_MAIN_HAND)
        slot = TSFP_MAIN_HAND_SLOT;
    else if (weapon_record == player_address + TSFP_PLAYER_OFF_HAND)
        slot = TSFP_OFF_HAND_SLOT;
    else if (slot < 0)
        slot = actor_hand_slot(&vm, entity, weapon_record);
    if (slot < 0 || !weapon_record)
        return;
    const unsigned hand = kept.slot_hand[slot];
    const uint32_t weapon_id = guest_word(&vm, weapon_record + 0x30u);
    const int mounted = current_weapon_bore((unsigned)slot, entity, weapon_id);
    if (kept.hand_move_entity[hand] != entity ||
        kept.frame - kept.hand_move_frame[hand] > TSFP_SHOT_POSE_FRAMES ||
        !tsfp_xr_touch_tracked(hand)) {
        static unsigned reported;
        if (reported++ < 4u)
            fprintf(stderr, "[6DoF] Projectile has no current pose for the %s hand\n", hand_names[hand]);
        return;
    }

    XrMatrix4x4f camera_from_world, world_from_camera;
    if (!read_camera_view(&vm, player, &camera_from_world))
        return;

    enum { ORIGIN, RAY_ORIGIN, MUZZLE, RAY, TRACER, PROJECTILE, VECTOR_COUNT };
    static const uint32_t local_offsets[VECTOR_COUNT] = { 0x70, 0x60, 0x98, 0x44, 0x50, 0x84 };
    void *vectors[VECTOR_COUNT];
    XrVector3f values[VECTOR_COUNT];
    for (unsigned i = 0; i < VECTOR_COUNT; ++i) {
        const uint32_t address = word(args + (5u + i) * sizeof(uint32_t));
        if (address != stack + local_offsets[i]) {
            static int reported;
            if (!reported) {
                reported = 1;
                fprintf(stderr, "[6DoF] Unexpected projectile argument %u at stack offset %x\n",
                        5u + i, address - stack);
            }
            return;
        }
        vectors[i] = vm.resolve(vm.host, address, sizeof values[i], 1);
        if (!vectors[i])
            return;
        memcpy(&values[i], vectors[i], sizeof values[i]);
    }

    XrMatrix4x4f_Invert(&world_from_camera, &camera_from_world);
    XrVector3f muzzle;
    if (mounted) {
        XrVector3f direction;
        tsfp_weapon_world_bore(&world_from_camera, kept.shots[slot].muzzle, kept.shots[slot].direction,
                               &muzzle, &direction);
        tsfp_weapon_aim_shot(direction, &values[RAY]);
    } else {
        XrMatrix4x4f moved_camera, move;
        XrMatrix4x4f_Multiply(&moved_camera, &kept.hand_move[hand], &camera_from_world);
        XrMatrix4x4f_Multiply(&move, &world_from_camera, &moved_camera);
        const XrMatrix4x4f rotate = linear_part(&move);
        XrMatrix4x4f_TransformVector3f(&muzzle, &move, &values[MUZZLE]);
        for (unsigned i = RAY; i < VECTOR_COUNT; ++i) {
            const XrVector3f direction = values[i];
            XrMatrix4x4f_TransformVector3f(&values[i], &rotate, &direction);
        }
    }
    values[ORIGIN] = values[RAY_ORIGIN] = values[MUZZLE] = muzzle;
    for (unsigned i = 0; i < VECTOR_COUNT; ++i)
        if (!isfinite(values[i].x) || !isfinite(values[i].y) || !isfinite(values[i].z))
            return;
    for (unsigned i = 0; i < VECTOR_COUNT; ++i)
        memcpy(vectors[i], &values[i], sizeof values[i]);

    static unsigned reported;
    if (reported < 12u) {
        ++reported;
        fprintf(stderr, "[6DoF] Projectile from %s hand: muzzle %.3f %.3f %.3f, ray %.3f %.3f %.3f\n",
                hand_names[hand], muzzle.x, muzzle.y, muzzle.z,
                values[RAY].x, values[RAY].y, values[RAY].z);
    }
}

int tsfp_stereo_flash_begin(uint32_t stack)
{
    const TsfpModelRole previous = avatar.role;
    if (!tsfp_stereo_weapon_projection() || !tsfp_stereo_immersive_player())
        return previous;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t player_address = guest_word(&vm, 0x007B0C7Cu);
    const uint8_t *player = first_person_player(&vm);
    const uint32_t record = guest_word(&vm, stack + 8u);
    const int slot = player ? actor_hand_slot(&vm, word(player + 0x14u), record) : -1;
    if (slot >= 0) {
        const uint32_t viewmodel = guest_word(&vm, stack + 4u);
        const unsigned hand = kept.slot_hand[slot];
        if (viewmodel == player_address + TSFP_PLAYER_MAIN_HAND)
            avatar.flash_weapon = avatar.view_weapon[TSFP_MAIN_HAND_SLOT];
        else if (viewmodel == player_address + TSFP_PLAYER_OFF_HAND)
            avatar.flash_weapon = avatar.view_weapon[TSFP_OFF_HAND_SLOT];
        if (avatar.flash_weapon)
            avatar.role = hand == TSFP_AVATAR_LEFT ? TSFP_MODEL_LEFT_FLASH : TSFP_MODEL_RIGHT_FLASH;
    }
    return previous;
}

void tsfp_stereo_flash_end(int previous)
{
    avatar.flash_weapon = 0;
    avatar.role = (TsfpModelRole)previous;
}

void tsfp_stereo_note_body_draw(void)
{
    static unsigned reported;
    const unsigned bit = 1u << avatar.posed_hands;
    if (!(reported & bit)) {
        reported |= bit;
        fprintf(stderr, "[6DoF] Submitted avatar geometry in stereo (tracked hand mask %u)\n",
                avatar.posed_hands);
    }
}

enum { TSFP_RESTORE_PALETTE = 0x100, TSFP_CAPTURE_BODY = 0x200 };

/* Actor hand slots do not name a side. The game draws an attachment at
 * the animated hand holding it, before the pose replaces that hand. */
static unsigned animated_weapon_hand(const uint8_t *palette, unsigned fallback)
{
    if (!palette || !avatar.camera_palette || !avatar.rig_sided)
        return fallback;
    const XrVector3f grip = palette_position(palette, 0);
    unsigned nearest = fallback;
    float best = 0.5f;
    for (unsigned hand = 0; hand < TSFP_AVATAR_HANDS; ++hand) {
        const unsigned matrix = avatar.rig[hand].matrix[TSFP_ARM_HAND];
        const int saved = avatar.palette_patched && (avatar.posed_hands & (1u << hand));
        const XrVector3f wrist = saved ? tsfp_matrix_position(&avatar.saved[hand][TSFP_ARM_HAND]) :
            palette_position(avatar.camera_palette, matrix);
        XrVector3f gap;
        XrVector3f_Sub(&gap, &grip, &wrist);
        const float distance = XrVector3f_Length(&gap);
        if (distance < best) {
            best = distance;
            nearest = hand;
        }
    }
    return nearest;
}

static _Thread_local unsigned model_depth, overlay_depth, first_person_weapon_depth;
static _Thread_local int screen_effects;

/* 14FF20 draws the per-view HUD. Screen effects run outside this scope. */
void tsfp_stereo_overlay(int begin)
{
    if (begin)
        ++overlay_depth;
    else if (overlay_depth)
        --overlay_depth;
}

int tsfp_stereo_hud_draw(void)
{
    return overlay_depth != 0;
}

void tsfp_stereo_screen_effects(int routine)
{
    screen_effects = routine;
}

int tsfp_stereo_screen_effect_draw(void)
{
    return screen_effects;
}

void tsfp_stereo_first_person_weapon_pass(int begin)
{
    if (begin)
        ++first_person_weapon_depth;
    else if (first_person_weapon_depth)
        --first_person_weapon_depth;
}

int tsfp_stereo_legacy_weapon_draw(void)
{
    return first_person_weapon_depth && !model_depth && !overlay_depth &&
        avatar.role == TSFP_MODEL_DEFAULT && tsfp_stereo_weapon_projection();

}

int tsfp_stereo_model_begin(uint32_t instance, uint32_t node, uint32_t view)
{
    ++model_depth;
    const TsfpModelRole previous = avatar.role;
    avatar.role = TSFP_MODEL_DEFAULT;
    if (!instance)
        return previous;
    if (instance == avatar.body) {
        static unsigned reported;
        const unsigned bit = 1u << avatar_world_pass();
        if (!(reported & bit)) {
            reported |= bit;
            fprintf(stderr, "[6DoF] Body model drawn in %s pass, node %u view %u\n",
                    avatar_world_pass() ? "world" : "another", node, view);
        }
    }
    if (instance == avatar.body && avatar_world_pass()) {
        if (!avatar.palette_patched && word(avatar.active_header) != avatar.header) {
            tsfp_guest_vm vm;
            xbox_GetGuestVirtualMemory(&vm);
            identify_body_rig(&vm);
        }
        if (!avatar.head || node >= avatar.body_count) {
            avatar.role = TSFP_MODEL_HIDDEN;
            return previous;
        }
        const int first_node = !avatar.pose_prepared;
        if (first_node) {
            prepare_avatar_pose(view);
            avatar.pose_prepared = 1;
        }
        const int visible = !tsfp_avatar_hide_head(avatar.body_nodes, avatar.body_count, avatar.head, node);
        avatar.role = visible ? TSFP_MODEL_BODY : TSFP_MODEL_HIDDEN;
        if (!visible)
            return previous;
        int restore = previous;
        if (first_node) {
            tsfp_vk_avatar_capture_begin();
            restore |= TSFP_CAPTURE_BODY;
        }
        if (avatar.posed_hands && !avatar.palette_patched) {
            exchange_avatar_palette(1);
            restore |= TSFP_RESTORE_PALETTE;
        }
        return restore;
    } else if ((instance == held.model[0] || instance == held.model[1]) && avatar.body && avatar_world_pass()) {
        if (!avatar.pose_prepared) {
            prepare_avatar_pose(view);
            avatar.pose_prepared = 1;
        }
        const unsigned slot = instance == held.model[TSFP_MAIN_HAND_SLOT] ?
            TSFP_MAIN_HAND_SLOT : TSFP_OFF_HAND_SLOT;
        const unsigned hand = kept.slot_hand[slot];
        tsfp_guest_vm vm;
        xbox_GetGuestVirtualMemory(&vm);
        const uint8_t *palette = model_camera_palette(&vm, instance, view);
        move_held_weapon(hand, animated_weapon_hand(palette, hand));
        if (avatar.view_weapon[slot] && (avatar.posed_hands & avatar.wrist_valid & (1u << hand)) &&
            (tsfp_weapon_profile(avatar.weapon_id[slot]) || view_arms_supported(&vm))) {
            if (avatar.weapon_id[slot] == TSFP_WEAPON_KRUGER && palette && !(held.pistol_poses & (1u << hand))) {
                XrMatrix4x4f root;
                memcpy(&root, palette, sizeof root);
                XrMatrix4x4f_Multiply(&held.pistol_root[hand], &held.correction[hand], &root);
                held.pistol_poses |= 1u << hand;
            }
            avatar.role = TSFP_MODEL_HIDDEN;
        } else {
            avatar.role = hand == TSFP_AVATAR_LEFT ? TSFP_MODEL_LEFT_HELD : TSFP_MODEL_RIGHT_HELD;
            held.drawn_slots |= 1u << slot;
        }
    } else if (tsfp_stereo_weapon_projection()) {
        if (instance == avatar.arms) {
            avatar.role = TSFP_MODEL_LEGACY_ARMS;
        } else {
            for (unsigned slot = 0; slot < TSFP_AVATAR_HANDS; ++slot) {
                if (instance == avatar.shield[slot]) {
                    const unsigned hand = kept.slot_hand[slot];
                    if (avatar.posed_hands & (1u << hand))
                        avatar.role = hand == TSFP_AVATAR_LEFT ? TSFP_MODEL_LEFT_SHIELD : TSFP_MODEL_RIGHT_SHIELD;
                    break;
                }
                if (instance != avatar.view_weapon[slot])
                    continue;
                const unsigned hand = kept.slot_hand[slot];
                if (held.drawn_slots & (1u << slot))
                    avatar.role = TSFP_MODEL_HIDDEN;
                else
                    avatar.role = hand == TSFP_AVATAR_LEFT ? TSFP_MODEL_LEFT_WEAPON : TSFP_MODEL_RIGHT_WEAPON;
                break;
            }
        }
    }
    return previous;
}

void tsfp_stereo_model_end(int previous)
{
    if (model_depth)
        --model_depth;
    if (previous & TSFP_RESTORE_PALETTE)
        exchange_avatar_palette(0);
    if (previous & TSFP_CAPTURE_BODY)
        tsfp_vk_avatar_capture_end();
    avatar.role = (TsfpModelRole)(previous & ~(TSFP_RESTORE_PALETTE | TSFP_CAPTURE_BODY));
}

TsfpModelRole tsfp_stereo_model_role(void)
{
    return avatar.role;
}

static const uint8_t *entity_from_handle(const tsfp_guest_vm *vm, uint32_t handle, uint32_t bytes)
{
    const uint32_t table = guest_word(vm, 0x0072f5e8u);
    uint32_t address;
    if (bytes < 0x30u || !tsfp_uplink_entity_address(table, handle, &address))
        return NULL;
    const uint8_t *entity = vm->resolve(vm->host, address, bytes, 0);
    return entity && word(entity + 0x2cu) == handle ? entity : NULL;
}

int tsfp_stereo_mounted_gun(void)
{
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint8_t *player = local_player(&vm, guest_word(&vm, 0x007B0C7Cu));
    if (!player)
        return 0;
    const uint32_t mode = word(player + 0x114u);
    if (mode == 0x80u)
        return 1; /* stationary and remotely controlled turrets */
    if (mode != 0x400u)
        return 0;

    /* The vehicle camera mode also covers drivers and passengers. Check
     * the player's seat before routing both axes to the Xbox gun controls. */
    const uint32_t owner = word(player + 0x14u);
    const uint8_t *entity = vm.resolve(vm.host, owner, 0x80u, 0);
    if (!entity)
        return 0;
    const uint32_t actor = word(entity + 0x7cu);
    const uint8_t *vehicle = entity_from_handle(&vm, actor ? guest_word(&vm, actor + 0xc08u) : 0, 0x80u);
    const uint8_t *data = vehicle ? vm.resolve(vm.host, word(vehicle + 0x7cu), 0x168u, 0) : NULL;
    const uint32_t owner_handle = word(entity + 0x2cu);
    if (!data || !owner_handle || word(data + 0x140u) > 4u)
        return 0;
    for (unsigned i = 0; i < word(data + 0x140u); ++i) {
        const uint8_t *seat = data + 0x70u + i * 0x34u;
        if (word(seat) == owner_handle)
            return word(seat + 0x30u) == 3u || word(seat + 0x30u) == 4u;
    }
    return 0;
}

void tsfp_stereo_apply_snap_turn(void)
{
    const int degrees = tsfp_xr_take_snap_turn();
    if (!degrees)
        return;
    float projection[16];
    if (!tsfp_stereo_world_projection(projection))
        return;
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t address = guest_word(&vm, 0x007B0C7Cu);
    unsigned char *player = vm.resolve(vm.host, address, 0x118u, 1);
    if (!player || word(player + 4) != 0)
        return;
    const uint32_t mode = word(player + 0x114u);
    if (mode != 2 && mode != 4 && mode != 0x800)
        return;
    float yaw;
    memcpy(&yaw, player + 0x100u, sizeof yaw);
    if (!isfinite(yaw))
        return;
    yaw = remainderf(yaw - degrees, 360.0f);
    memcpy(player + 0x100u, &yaw, sizeof yaw);
}

void tsfp_stereo_probe_camera(uint32_t caller)
{
    /* The US Xbox main player camera builds its lens at these two sites.
     * The PC display adapter uses the same sites to exclude scripted views. */
    if (!enabled || (caller != 0x000CCBEDu && caller != 0x000CDD35u))
        return;

    /* This is the player's projection setup, before world geometry. Start
     * the XR frame here and reuse the same poses at presentation. */
    tsfp_xr_prepare_scene_frame();
    if (camera_calls++ % 300u != 0)
        return;

    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    if (guest_word(&vm, 0x00790950u) != 1u ||
        (int32_t)guest_word(&vm, 0x0079094Cu) > 100 ||
        (guest_word(&vm, 0x007DE458u) & 0x20000000u))
        return;

    const uint32_t actor_address = guest_word(&vm, 0x007B0C7Cu);
    const uint32_t camera_address = guest_word(&vm, 0x007B0CC0u);
    const unsigned char *actor = vm.resolve(vm.host, actor_address, 0x118u, 0);
    const unsigned char *object = vm.resolve(vm.host, camera_address, 0x288u, 0);
    if (!actor || !object || word(actor + 4) != 0 || word(actor + 8) != camera_address)
        return;
    const uint32_t mode = word(actor + 0x114u);
    if (mode != 2 && mode != 4 && mode != 0x800)
        return;

    const void *projection = vm.resolve(vm.host, word(object + 0x204u), sizeof camera.projection, 0);
    const void *view = vm.resolve(vm.host, word(object + 0x208u), sizeof camera.view, 0);
    if (!projection || !view)
        return;
    memcpy(camera.projection, projection, sizeof camera.projection);
    memcpy(camera.view, view, sizeof camera.view);
    memcpy(camera.position, object + 0x27cu, sizeof camera.position);
    camera.caller = caller;
}

static void log_matrix(const char *name, const float matrix[16])
{
    fprintf(stderr, "[M4] %s %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g "
                    "%.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n",
            name, matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5],
            matrix[6], matrix[7], matrix[8], matrix[9], matrix[10], matrix[11],
            matrix[12], matrix[13], matrix[14], matrix[15]);
}

int tsfp_xr_locate_stereo(XrSession session, XrSpace local, XrTime display_time,
                          XrView eyes[TSFP_XR_EYES])
{
    XrViewLocateInfo locate = { .type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = display_time, .space = local };
    XrViewState state = { .type = XR_TYPE_VIEW_STATE };
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye)
        eyes[eye] = (XrView){ .type = XR_TYPE_VIEW };
    uint32_t count = 0;
    const XrViewStateFlags valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    const XrResult result = xrLocateViews(session, &locate, &state, TSFP_XR_EYES, &count, eyes);
    return XR_SUCCEEDED(result) && count == TSFP_XR_EYES && (state.viewStateFlags & valid) == valid;
}

void tsfp_stereo_probe_frame(XrSession session, XrSpace local, XrTime display_time,
                            const XrPosef *anchor, const XrView *eyes, uint32_t frame)
{
    if (!enabled)
        return;
    const uint32_t caller = camera.caller;
    camera.caller = 0;
    if (!anchor || (!caller && frame % 300u != 0))
        return;

    XrView sampled[TSFP_XR_EYES];
    if (!eyes) {
        if (!tsfp_xr_locate_stereo(session, local, display_time, sampled))
            return;
        eyes = sampled;
    }

    const XrVector3f left = eyes[0].pose.position, right = eyes[1].pose.position;
    const float dx = right.x - left.x, dy = right.y - left.y, dz = right.z - left.z;
    fprintf(stderr, "[M4] frame %u display time %lld eye separation %.4f m caller %08x\n",
            frame, (long long)display_time, sqrtf(dx * dx + dy * dy + dz * dz), caller);
    for (unsigned eye = 0; eye < TSFP_XR_EYES; ++eye) {
        /* Diagnostic distances are in meters. The game's depth range and
         * world scale are recorded separately for calibration. */
        XrMatrix4x4f clip = tsfp_eye_clip(anchor, &eyes[eye], 0.05f, 100.0f);
        log_matrix(eye == 0 ? "left eye clip" : "right eye clip", clip.m);
    }
    if (caller) {
        fprintf(stderr, "[M4] guest position %.6g %.6g %.6g\n",
                camera.position[0], camera.position[1], camera.position[2]);
        log_matrix("guest view", camera.view);
        log_matrix("guest projection", camera.projection);
        XrMatrix4x4f projection;
        memcpy(projection.m, camera.projection, sizeof projection.m);
        float near_z, far_z;
        if (tsfp_xbox_camera_planes(&projection, &near_z, &far_z))
            fprintf(stderr, "[M4] Xbox camera clip planes %.6g %.6g in game units\n", near_z, far_z);
    }
}
