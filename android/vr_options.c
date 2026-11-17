#include "vr_options.h"
#include "xr.h"
#include "recomp/gen/recomp_funcs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

/* Callback slots in US Xbox INT3 padding, also used by the PC Options adapter.
 * This Android target does not activate the PC menu callbacks. */
enum { VR_OPEN = 0x12008u, VR_BUILD, VR_TICK, VR_BACK = 0x1200cu };
enum { TURN_TYPE, SNAP_ANGLE };
enum { SMOOTH_TURN, SNAP_TURN, TURN_MODE_COUNT };
enum { TITLE, MODE, SMOOTH, SNAP, ANGLE, ANGLE_30, ANGLE_45, ANGLE_60, SAVE_ERROR };
static const unsigned angles[] = { 30, 45, 60 };
static const char captions[][32] = {
    [TITLE] = "VR Options", [MODE] = "Turn Type", [SMOOTH] = "Smooth", [SNAP] = "Snap",
    [ANGLE] = "Snap Angle", [ANGLE_30] = "30 degrees", [ANGLE_45] = "45 degrees",
    [ANGLE_60] = "60 degrees", [SAVE_ERROR] = "Could not save VR options"
};

static const char *settings_path;
static unsigned turn_mode, angle_index;
static int save_failed;
static uint32_t descriptor, parent, previous, mode_row, angle_row;

void tsfp_vr_options_init(const char *path, const char *turn_override)
{
    settings_path = path;
    turn_mode = SMOOTH_TURN;
    angle_index = 0;
    save_failed = 0;
    FILE *file = fopen(path, "r");
    if (file) {
        unsigned mode, degrees;
        char extra;
        if (fscanf(file, "%1u %2u %c", &mode, &degrees, &extra) == 2 && mode < TURN_MODE_COUNT) {
            for (unsigned i = 0; i < sizeof angles / sizeof angles[0]; ++i) {
                if (degrees == angles[i]) {
                    turn_mode = mode;
                    angle_index = i;
                    break;
                }
            }
        }
        fclose(file);
    }
    if (*turn_override)
        turn_mode = strcmp(turn_override, "snap") == 0 ? SNAP_TURN : SMOOTH_TURN;
    tsfp_xr_set_snap_degrees(turn_mode == SNAP_TURN ? (int)angles[angle_index] : 0);
}

static int save_options(unsigned mode, unsigned angle)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s.XXXXXX", settings_path) >= (int)sizeof path)
        return 0;
    const int fd = mkstemp(path);
    if (fd < 0)
        return 0;
    FILE *file = fdopen(fd, "w");
    int written = 0;
    if (file) {
        written = fprintf(file, "%u %u\n", mode, angles[angle]) > 0;
        written = fclose(file) == 0 && written;
    } else {
        close(fd);
    }
    if (written && rename(path, settings_path) == 0)
        return 1;
    unlink(path);
    return 0;
}

/* Native menu helpers use the guest cdecl stack. Preserve the caller's
 * registers while running the extra row builder or a menu transition. */
static uint32_t guest_call(void (*fn)(void), const uint32_t *args, unsigned count)
{
    const uint32_t eax = g_eax, ebx = g_ebx, ecx = g_ecx, edx = g_edx;
    const uint32_t esp = g_esp, esi = g_esi, edi = g_edi, ebp = g_ebp, seh = g_seh_ebp;
    const tsfp_status_flags flags = g_status_flags;
    const RecompXmm xmm[] = { g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7 };
    const int fp_top = g_fp_top, fp_cmp = g_fp_cmp, df = g_df;
    const uint16_t fp_cc = g_fp_cc, fp_control = g_fp_control_word;
    double fp[8];
    memcpy(fp, g_fp_stack, sizeof fp);
    while (count)
        PUSH32(g_esp, args[--count]);
    PUSH32(g_esp, VR_OPEN);
    fn();
    const uint32_t result = g_eax;
    g_eax = eax; g_ebx = ebx; g_ecx = ecx; g_edx = edx;
    g_esp = esp; g_esi = esi; g_edi = edi; g_ebp = ebp; g_seh_ebp = seh;
    g_status_flags = flags;
    g_xmm0 = xmm[0]; g_xmm1 = xmm[1]; g_xmm2 = xmm[2]; g_xmm3 = xmm[3];
    g_xmm4 = xmm[4]; g_xmm5 = xmm[5]; g_xmm6 = xmm[6]; g_xmm7 = xmm[7];
    g_fp_top = fp_top; g_fp_cmp = fp_cmp; g_df = df;
    g_fp_cc = fp_cc; g_fp_control_word = fp_control;
    memcpy(g_fp_stack, fp, sizeof fp);
    return result;
}

#define CALL(fn, ...) guest_call((fn), (const uint32_t[]){__VA_ARGS__}, \
                                  sizeof((const uint32_t[]){__VA_ARGS__}) / sizeof(uint32_t))

static uint32_t caption(unsigned index)
{
    /* Descriptor: guest build, update, draw and destroy addresses. */
    return descriptor + 16u + index * sizeof captions[0];
}

void tsfp_quest_vr_menu_button(void (*original)(void))
{
    const uint32_t site = MEM32(g_esp), callback = MEM32(g_esp + 20u);
    const int settings = site == 0x002CC116u && callback == 0x002CBE50u;
    const int pause_options = site == 0x002F561Cu && callback == 0x002EC1A0u;
    if (tsfp_xr_enabled() && (settings || pause_options)) {
        if (!descriptor) {
            descriptor = xbox_HeapAlloc(16u + sizeof captions, 4);
            if (descriptor) {
                MEM32(descriptor) = VR_BUILD;
                MEM32(descriptor + 4u) = VR_TICK;
                MEM32(descriptor + 8u) = MEM32(descriptor + 12u) = 0;
                memcpy((void *)(g_xbox_mem_offset + caption(0)), captions, sizeof captions);
            }
        }
        if (descriptor)
            CALL(original, caption(TITLE), UINT32_MAX, 0, 0, VR_OPEN, 0);
    }
    original();
}

static uint32_t add_choice(unsigned label, unsigned id, unsigned first, unsigned count, unsigned value)
{
    const uint32_t row = CALL(sub_00079D60_gen, caption(label), id, 0, 0, 0, 0);
    CALL(sub_0007D480, 0x8400u, 0);
    for (unsigned i = 0; i < count; ++i)
        CALL(sub_000717D0, caption(first + i));
    CALL(sub_00075430, row, value);
    return row;
}

static void build_menu(void)
{
    const uint32_t menu = MEM32(g_esp + 4u);
    CALL(sub_00075B70, 0xa000u, caption(TITLE), 0x429c0000u, 0, VR_BACK); /* Width 78.0f. */
    CALL(sub_00071B10, 1, 0x3f99999au, 1, 1); /* Native row spacing 1.2f. */
    CALL(sub_00071C50, 0x26200u);
    mode_row = add_choice(MODE, TURN_TYPE, SMOOTH, TURN_MODE_COUNT, turn_mode);
    angle_row = turn_mode == SNAP_TURN ? add_choice(ANGLE, SNAP_ANGLE, ANGLE_30, 3, angle_index) : 0;
    if (save_failed)
        CALL(sub_00079D60_gen, caption(SAVE_ERROR), UINT32_MAX, 4, 0, 0, 0);
    CALL(sub_000771D0, menu);
    g_esp += 4u;
}

static void tick_menu(void)
{
    const uint32_t menu = MEM32(g_esp + 4u);
    const unsigned mode = mode_row ? MEM32(mode_row + 0xc8u) : turn_mode;
    const unsigned angle = angle_row ? MEM32(angle_row + 0xc8u) : angle_index;
    if (mode < TURN_MODE_COUNT && angle < sizeof angles / sizeof angles[0] &&
        (mode != turn_mode || angle != angle_index)) {
        const int old_failure = save_failed;
        const int mode_changed = mode != turn_mode;
        save_failed = !save_options(mode, angle);
        if (!save_failed) {
            turn_mode = mode;
            angle_index = angle;
            tsfp_xr_set_snap_degrees(turn_mode == SNAP_TURN ? (int)angles[angle_index] : 0);
        }
        if (mode_changed || old_failure != save_failed) {
            CALL(sub_00078980, menu);
            /* The guest refresh restores old values by row ID. */
            CALL(sub_00075430, mode_row, turn_mode);
            if (angle_row)
                CALL(sub_00075430, angle_row, angle_index);
            CALL(sub_000770E0, menu, mode_changed ? TURN_TYPE : SNAP_ANGLE);
        } else if (save_failed) {
            CALL(sub_00075430, mode_row, turn_mode);
            if (angle_row)
                CALL(sub_00075430, angle_row, angle_index);
        }
    }
    g_esp += 4u;
}

static void open_menu(void)
{
    const uint32_t source = MEM32(g_esp + 4u);
    parent = MEM32(source + 0x2cu);
    previous = MEM32(source + 4u);
    CALL(sub_00079B70, parent, descriptor);
    g_esp += 4u;
}

static void back_menu(void)
{
    CALL(sub_00079B70, parent, previous);
    g_esp += 4u;
}

void (*tsfp_quest_vr_menu_lookup(uint32_t address))(void)
{
    switch (address) {
    case VR_OPEN: return open_menu;
    case VR_BUILD: return build_menu;
    case VR_TICK: return tick_menu;
    case VR_BACK: return back_menu;
    default: return NULL;
    }
}
