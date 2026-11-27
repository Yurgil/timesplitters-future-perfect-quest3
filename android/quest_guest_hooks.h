#ifndef TSFP_QUEST_GUEST_HOOKS_H
#define TSFP_QUEST_GUEST_HOOKS_H

#include "pc_native_numeric.h"

void tsfp_quest_projectile(void);
void tsfp_quest_grenade_constructor(void);
void tsfp_quest_cart_actor_projectile(void);
void tsfp_quest_trace_projectile(void);
void tsfp_quest_muzzle_flash(void);
void tsfp_quest_muzzle_flash_position(void);
void tsfp_quest_shot_effect_position(void);
void tsfp_quest_weapon_particles(void);
void tsfp_quest_legacy_reticle(void);
void tsfp_quest_alternate_reticle(void);
void tsfp_quest_mech_reticle(void);
void tsfp_quest_turret_look(void);
void tsfp_quest_vehicle_update(void);
void tsfp_quest_cart_visible(void);
void tsfp_quest_hud(void);
void tsfp_quest_objective_notice(void);
void tsfp_quest_screen_effects(void);
void tsfp_quest_story_screen_effects(void);
void tsfp_quest_world_screen_effects(void);
void tsfp_quest_late_screen_effects(void);
void tsfp_quest_dispersion_impact(void);
void tsfp_quest_time_grenade_effect(void);
void tsfp_quest_mag_charger_shot(void);
void tsfp_quest_mag_charger_vision(void);
void tsfp_quest_ghost_actor(void);
void tsfp_quest_ghost_reveal(void);
void tsfp_quest_first_person_weapon_pass(void);
void tsfp_quest_portal_visibility(void);
void tsfp_quest_object_visibility(void);
void tsfp_quest_object_occlusion(void);
void tsfp_quest_uplink_grab(void);
void tsfp_quest_uplink_legacy_beams(void);
void tsfp_quest_uplink_map(void);
void tsfp_quest_uplink_map_source(void);
int tsfp_quest_uplink_physics_begin(uint32_t callback);
void tsfp_quest_uplink_physics_end(void);

/* Skip cart hooks outside the Catacombs story scene. This runs at guest
 * call sites that also serve ordinary object culling and NPC fire. */
static inline int tsfp_quest_catacombs_scene(void)
{
    const uint32_t scene = *(const uint32_t *)((uintptr_t)0x0079094cu + g_xbox_mem_offset);
    if (scene != 17u)
        return 0;
    const uint32_t mode = *(const uint32_t *)((uintptr_t)0x0078ad20u + g_xbox_mem_offset);
    return mode == 4u || mode == 14u;
}

/* These call sites are in the US Xbox executable. Keep the PC numeric
 * policy for all other calls in this generated translation unit. */
typedef void (*TsfpGuestCallee)(void);
static inline __attribute__((always_inline)) TsfpGuestCallee tsfp_quest_guest_callee(
    uint32_t site, TsfpGuestCallee original)
{
    switch (site) {
    case 0x00089BD9u: return tsfp_quest_portal_visibility;
    case 0x00086579u:
    case 0x00086DE4u: return tsfp_quest_object_visibility;
    case 0x000A249Eu: return tsfp_quest_object_occlusion;
    case 0x000A1C42u:
    case 0x000A1C75u:
    case 0x000A1CA5u:
    case 0x000A2AB3u:
        if (tsfp_quest_catacombs_scene())
            return tsfp_quest_cart_visible;
        break;
    case 0x001C0B97u: return tsfp_quest_projectile;
    case 0x001B79F0u: return tsfp_quest_grenade_constructor;
    case 0x001BC451u:
        if (tsfp_quest_catacombs_scene())
            return tsfp_quest_cart_actor_projectile;
        break;
#ifdef TSFP_TRACE_CART
    case 0x001BC876u:
    case 0x002457C2u: return tsfp_quest_trace_projectile;
#endif
    case 0x001B92B8u:
    case 0x001B92D1u: return tsfp_quest_muzzle_flash;
    case 0x001B86A2u:
    case 0x001B8723u:
    case 0x001B87C3u: return tsfp_quest_muzzle_flash_position;
    case 0x001BC470u:
    case 0x001C0EC0u: return tsfp_quest_shot_effect_position;
    case 0x0011219Au: return tsfp_quest_weapon_particles;
    case 0x001CEEDEu: return tsfp_quest_legacy_reticle;
    case 0x0014D324u: return tsfp_quest_alternate_reticle;
    case 0x0014F1C7u: return tsfp_quest_mech_reticle;
    case 0x000D6275u: return tsfp_quest_turret_look;
    case 0x000D648Au: return tsfp_quest_vehicle_update;
    case 0x001CE9D1u: return tsfp_quest_hud;
    case 0x0014D655u: return tsfp_quest_objective_notice;
    case 0x001CED85u:
    case 0x001CEF14u: return tsfp_quest_screen_effects;
    case 0x001CED8Au: return tsfp_quest_story_screen_effects;
    case 0x001CEED0u: return tsfp_quest_world_screen_effects;
    case 0x001CE9A9u: return tsfp_quest_late_screen_effects;
    case 0x000FECF0u:
    case 0x000FED73u: return tsfp_quest_dispersion_impact;
    case 0x000FECFEu:
    case 0x000FED81u: return tsfp_quest_time_grenade_effect;
    case 0x000FED98u: return tsfp_quest_mag_charger_shot;
    case 0x0014D31Fu: return tsfp_quest_mag_charger_vision;
    case 0x00126421u: return tsfp_quest_ghost_actor;
    case 0x0012644Eu: return tsfp_quest_ghost_reveal;
    case 0x001CEEE8u: return tsfp_quest_first_person_weapon_pass;
    case 0x001C57B4u: return tsfp_quest_uplink_grab;
    case 0x001C5AD7u: return tsfp_quest_uplink_map;
    case 0x001CE987u: return tsfp_quest_uplink_map_source;
    case 0x001CEECBu: return tsfp_quest_uplink_legacy_beams;
    }
    return RECOMP_DIRECT_CALLEE_AT(site, original);
}

#undef RECOMP_DIRECT_CALLEE_AT
#define RECOMP_DIRECT_CALLEE_AT(site, callee) tsfp_quest_guest_callee((site), (callee))

#endif
