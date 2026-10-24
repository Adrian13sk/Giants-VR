// Each hero's clip for each PlayerAnim (index = PlayerAnim), shared by the player's body and the
// skirmish's computer players.
#pragma once

#include "game/player.h"

namespace gvr {

inline const char* const kMeccClips[int(PlayerAnim::Count)] = {
    "mc_stand.anm", "mc_run.anm", "mc_hover.anm", "mc_hover.anm", "mc_land_soft.anm", "mc_sidestep.anm",
    "mc_skid.anm", "mc_thrust.anm", "mc_fly_left.anm", "mc_fly_right.anm", "mc_hover_back.anm",
    "mc_land_hard.anm", "mc_fly_cycle.anm", "mc_health_pup.anm", "mc_hover.anm", "mc_run.anm",
    "mc_stand.anm", "mc_stand.anm", "mc_stand.anm", "mc_stand.anm", "mc_throw_stuff.anm"};

// Delphi: no jetpack - jumps, falls, swims, turbo, sword swipes, spell.
inline const char* const kDelphiClips[int(PlayerAnim::Count)] = {
    "rp_stand.anm",     "rp_run.anm",        "rp_jump_cycle.anm", "rp_swim_treadwater.anm", "rp_land.anm",
    "rp_side_r.anm",    "rp_slide.anm",      "rp_runjump_cycle.anm", "rp_jump_cycle.anm",   "rp_jump_cycle.anm",
    "rp_jump_cycle.anm", "rp_land_hard.anm", "rp_fall.anm",       "rp_spell.anm",           "rp_swim_breast.anm",
    "rp_turbo.anm",     "rp_swipe_1.anm",    "rp_swipe_2.anm",    "rp_swipe_3.anm",         "rp_spell_arm_bolt.anm",
    "rp_spell_arm_bolt.anm"};

// Kabuto: no jetpack (he jumps); Inject = eating, Attack1/2 = right and left punches, Attack3 = the
// stomp, Cast = the roar.
inline const char* const kKabutoClips[int(PlayerAnim::Count)] = {
    "kb_stand.anm",     "kb_walk.anm",       "kb_jump_up_loop.anm", "kb_stand.anm",   "kb_land_light.anm",
    "kb_side_r.anm",    "kb_slide.anm",      "kb_run_jump_loop.anm", "kb_jump_up_loop.anm", "kb_jump_up_loop.anm",
    "kb_jump_up_loop.anm", "kb_land.anm",    "kb_fall.anm",         "kb_eat_small.anm", "kb_mud_walk.anm",
    "kb_run1.anm",      "kb_punch.anm",      "kb_punch_lft.anm",    "kb_stomp.anm",   "kb_roar.anm",
    "kb_throw.anm"};

}  // namespace gvr
