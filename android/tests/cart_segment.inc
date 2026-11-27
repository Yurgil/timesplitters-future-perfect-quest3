/* Included only in the Catacombs test APK. The original spawn action
 * constructs the cart and runs the game's boarding callback. */
enum { CART_TEST_SCENE = 17 };

#ifdef TSFP_CART_DIAGNOSTIC
#include <stdarg.h>

static void cart_test_log(int priority, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    va_start(args, format);
    __android_log_vprint(priority, "TSFP_CART", format, args);
    va_end(args);
}
#else
#define cart_test_log(priority, ...) __android_log_print(priority, "TSFP_CART", __VA_ARGS__)
#endif

static int cart_test_spawn(void)
{
    enum { ACTION_STRIDE = 0x48, MAX_ACTIONS = 4096 };
    tsfp_guest_vm vm;
    xbox_GetGuestVirtualMemory(&vm);
    const uint32_t actions = MEM32(0x0078cd90u), count = MEM32(0x0075cca8u);
    if (!actions || !count || count > MAX_ACTIONS ||
        !vm.resolve(vm.host, actions, count * ACTION_STRIDE, 1))
        return 0;

    uint32_t found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t action = actions + i * ACTION_STRIDE;
        const uint32_t raw = MEM32(action);
        if (!raw || !vm.resolve(vm.host, raw, 0xc8u, 0) ||
            MEM32(raw + 8u) != 3u || MEM32(raw + 0x10u) != TSFP_CATACOMBS_CART_MODEL ||
            MEM32(raw + 0x14u) != TSFP_CATACOMBS_CART_IDLE_TAG || MEM32(raw + 0x18u) != 0x230u)
            continue;
        if (found)
            return 0;
        found = action;
    }
    if (!found || MEM32(found + 0x2cu))
        return 0;

    /* Use the original action dispatcher with the runtime row already
     * loaded from Catacombs. Mirror 2378D0's state transition so the
     * script can't spawn the same cart again at the normal trigger. */
    const EffectRegisters saved = effect_registers();
    MEM32(found + 0x2cu) = 1;
    MEM32(found + 0x30u) = 0;
    MEM32(found + 0x38u) = MEM32(MEM32(found) + 0xa4u);
    g_eax = found;
    PUSH32(g_esp, 0);
    sub_00236070();
    restore_effect_registers(&saved);
    const uint32_t handle = MEM32(found + 0x24u);
    cart_test_log(handle ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                  "cart action executed action=%x handle=%x", found, handle);
    return 1;
}

static void cart_test_tick(void)
{
    static unsigned ready_updates;
    static uint32_t last_stage_tick;
    static int dispatched, boarded, reported_phase, entry_logged;
    const uint32_t scene = MEM32(0x0079094cu), type = MEM32(0x0078ad20u);
    const uint32_t players = MEM32(0x00790950u), stage_tick = MEM32(0x007de318u);
    if (stage_tick < last_stage_tick) {
        ready_updates = 0;
        dispatched = boarded = reported_phase = entry_logged = 0;
    }
    last_stage_tick = stage_tick;

    if (!entry_logged) {
        entry_logged = 1;
        cart_test_log(ANDROID_LOG_INFO, "player update scene=%u type=%u players=%u tick=%u",
                            scene, type, players, stage_tick);
    }
    const uint32_t player = MEM32(0x007b0c7cu);
    const uint32_t body = player ? MEM32(player + 0x14u) : 0;
    const uint32_t actor = body ? MEM32(body + 0x7cu) : 0;
    const uint32_t mode = player ? MEM32(player + 0x114u) : 0;
    if (dispatched) {
        if (!boarded && actor && mode == TSFP_CAMERA_CATACOMBS_CART) {
            const uint32_t cart = MEM32(actor + 0xc04u);
            if (cart && (MEM32(cart + 0x94u) & 0xffffu) == TSFP_CATACOMBS_CART_MODEL &&
                MEM32(cart + 0x1a0u) == 1u) {
                /* The player crossed the whole level in one update.
                 * Refresh the original positional and room caches at the
                 * seat so the world cull starts from the cart's tunnel. */
                const EffectRegisters saved = effect_registers();
                PUSH32(g_esp, 0);
                sub_000D4DB0();
                g_esp = saved.esp;
                PUSH32(g_esp, player);
                PUSH32(g_esp, 0);
                sub_000D2DE0();
                restore_effect_registers(&saved);
                boarded = 1;
                cart_test_log(ANDROID_LOG_INFO, "board ok mode=%#x cart=%x tag=%x weapon=%u room=%u cart_room=%u",
                              mode, cart, MEM32(cart + 0x60u), MEM32(actor + 0x94u),
                              MEM32(body + 0x34u), MEM32(cart + 0x34u));
            }
        }
        return;
    }

    const uint32_t context = MEM32(0x0075c0b0u);
    const int active = tsfp_stereo_world_scene();
    int phase = 0;
    if ((type != 4u && type != 14u) || players != 1u)
        phase = 1;
    else if (active != CART_TEST_SCENE)
        phase = 2;
    else if (!actor || MEM32(player + 4u) != 0 || mode != 2u || MEM32(actor) != body || !context)
        phase = 3;
    if (phase) {
        ready_updates = 0;
        if (phase != reported_phase) {
            reported_phase = phase;
            cart_test_log(ANDROID_LOG_INFO,
                "waiting phase=%d type=%u players=%u active=%d player=%x mode=%#x body=%x actor=%x context=%x",
                phase, type, players, active, player, mode, body, actor, context);
        }
        return;
    }

    /* The level data may become available a few updates after the player. */
    if (ready_updates++ % 30u != 0u)
        return;
    dispatched = cart_test_spawn();
    if (!dispatched && (reported_phase != 4 || ready_updates % 300u == 1u)) {
        reported_phase = 4;
        cart_test_log(ANDROID_LOG_WARN, "waiting for cart action table=%x count=%u",
                      MEM32(0x0078cd90u), MEM32(0x0075cca8u));
    }
}

/* 1CE850 updates the active player's simulation before drawing the
 * view. Run the shortcut while the guest's player context is selected. */
extern void __real_sub_000D5EB0(void);

void __wrap_sub_000D5EB0(void)
{
    if (MEM32(0x0079094cu) == CART_TEST_SCENE)
        cart_test_tick();
    __real_sub_000D5EB0();
}

extern void tsfp_stereo_cart_test_shot(uint32_t stack);
extern void __real_sub_001B7680(void);

void __wrap_sub_001B7680(void)
{
    tsfp_stereo_cart_test_shot(g_esp);
    __real_sub_001B7680();
}
