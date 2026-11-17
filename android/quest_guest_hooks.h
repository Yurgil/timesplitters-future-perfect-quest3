#ifndef TSFP_QUEST_GUEST_HOOKS_H
#define TSFP_QUEST_GUEST_HOOKS_H

#include "pc_native_numeric.h"

void tsfp_quest_projectile(void);
void tsfp_quest_muzzle_flash(void);
void tsfp_quest_muzzle_flash_position(void);
void tsfp_quest_shot_effect_position(void);
void tsfp_quest_weapon_particles(void);
void tsfp_quest_legacy_reticle(void);
void tsfp_quest_hud(void);
void tsfp_quest_objective_notice(void);
void tsfp_quest_screen_effects(void);
void tsfp_quest_story_screen_effects(void);
void tsfp_quest_world_screen_effects(void);
void tsfp_quest_late_screen_effects(void);
void tsfp_quest_first_person_weapon_pass(void);
void tsfp_quest_portal_visibility(void);
void tsfp_quest_object_visibility(void);
void tsfp_quest_object_occlusion(void);
void tsfp_quest_uplink_grab(void);
void tsfp_quest_uplink_legacy_beams(void);
void tsfp_quest_uplink_map(void);
int tsfp_quest_uplink_physics_begin(uint32_t callback);
void tsfp_quest_uplink_physics_end(void);

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
    case 0x001C0B97u: return tsfp_quest_projectile;
    case 0x001B92B8u:
    case 0x001B92D1u: return tsfp_quest_muzzle_flash;
    case 0x001B86A2u:
    case 0x001B8723u:
    case 0x001B87C3u: return tsfp_quest_muzzle_flash_position;
    case 0x001C0EC0u: return tsfp_quest_shot_effect_position;
    case 0x0011219Au: return tsfp_quest_weapon_particles;
    case 0x001CEEDEu: return tsfp_quest_legacy_reticle;
    case 0x001CE9D1u: return tsfp_quest_hud;
    case 0x0014D655u: return tsfp_quest_objective_notice;
    case 0x001CED85u:
    case 0x001CEF14u: return tsfp_quest_screen_effects;
    case 0x001CED8Au: return tsfp_quest_story_screen_effects;
    case 0x001CEED0u: return tsfp_quest_world_screen_effects;
    case 0x001CE9A9u: return tsfp_quest_late_screen_effects;
    case 0x001CEEE8u: return tsfp_quest_first_person_weapon_pass;
    case 0x001C57B4u: return tsfp_quest_uplink_grab;
    case 0x001C5AD7u: return tsfp_quest_uplink_map;
    case 0x001CEECBu: return tsfp_quest_uplink_legacy_beams;
    }
    return RECOMP_DIRECT_CALLEE_AT(site, original);
}

#undef RECOMP_DIRECT_CALLEE_AT
#define RECOMP_DIRECT_CALLEE_AT(site, callee) tsfp_quest_guest_callee((site), (callee))

#endif
