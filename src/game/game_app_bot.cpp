// (2026-10-29) Test hook GVR_MISSION_BOT=1: plays a mission's objectives the way a player would, with the real
// controls - so a test can show a mission is finishable by hand, not only marked complete (GVR_AUTOCOMPLETE, the
// campaign run). story3_prison_6 had passed every test that way while no player could finish it (no lift).
// Each half second it picks the nearest objective left: a place to reach, a character or Reaper to kill, a building
// to bring down, Vimp meat to hunt and bring. It walks there along the reach planner's path (the jetpack up banks), and in range it stops, points
// the weapon hand at the target and fires: Baz holds the trigger (pulses for one-shot guns), Delphi throws fire
// bolts. Not for Kabuto (eating, growing and gates have their own tests) nor rescues and races (their own
// autopilots: GVR_AUTO_RESCUE, GVR_SKI_AUTOPILOT).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"

namespace gvr {

bool GameApp::botOn() const {
    static const bool on = std::getenv("GVR_MISSION_BOT") != nullptr;
    return on && playMode_ && !title_ && hero_ != Hero::Kabuto && !cutsceneActive() && load_.phase == LoadPhase::Idle;
}

// Where objective i's target is now; false if it has none the bot can go for.
bool GameApp::botTargetOf(int i, Vec3& at, bool& shoot) const {
    const Objective& o = objectives_[size_t(i)];
    if (o.done) return false;
    switch (o.kind) {
        case 1:
            at = (o.boundsMin + o.boundsMax) * 0.5f;
            shoot = false;
            return true;
        case 2: {
            float best = 1e30f;
            bool found = false;
            for (const auto& e : enemies_.enemies)
                if (e.objective == i && e.state != EnemyState::Dead && length(e.pos - player_.position) < best) {
                    best = length(e.pos - player_.position);
                    at = e.pos + Vec3{0, 1.0f, 0};
                    found = true;
                }
            for (const auto& c : creatures_.creatures)
                if (c.state != CreatureState::Dead && c.slot >= 0 && size_t(c.slot) < chars_.size() && chars_[size_t(c.slot)].objective == i &&
                    length(c.pos - player_.position) < best) {
                    best = length(c.pos - player_.position);
                    at = c.pos + Vec3{0, 0.6f, 0};
                    found = true;
                }
            for (const auto& c : chars_)
                if (c.boss && c.hp > 0 && c.objective == i && length(c.pos - player_.position) < best) {
                    best = length(c.pos - player_.position);
                    at = c.pos + Vec3{0, 0.48f * c.bossH, 0};
                    found = true;
                }
            if (!found) at = o.boundsMin + Vec3{0, 1.0f, 0};
            shoot = true;
            return true;
        }
        case 5:
            if (o.item < 0 || size_t(o.item) >= destruct_.items.size() || !destruct_.items[size_t(o.item)].alive()) return false;
            {   // (low on it: a galleon's mast makes it "107 m tall"; its base can be the sea floor)
                const Destructible& d = destruct_.items[size_t(o.item)];
                at = d.base + Vec3{0, std::max(1.0f, std::min(6.0f, 0.4f * d.height)), 0};
                at.y = std::max(at.y, player_.seaLevel + 2.0f);
            }
            shoot = true;
            return true;
        case 4: {
            // Vimp meat for 2a: enough carried - to the trigger; meat lying about - walk over it; else hunt a grown Vimp.
            if (vimpMeat_ >= kVimpMeatNeeded) {
                at = (o.boundsMin + o.boundsMax) * 0.5f;
                shoot = false;
                return true;
            }
            float best = 1e30f;
            bool found = false;
            for (const auto& m : meat_)
                if (m.t > 0.5f && length(m.pos - player_.position) < std::min(best, 60.0f)) { best = length(m.pos - player_.position); at = m.pos; shoot = false; found = true; }
            if (found) return true;
            for (const auto& v : vimps_.vimps)
                if (v.state != VimpState::Dead && v.growth >= 1.0f && length(v.pos - player_.position) < best) {
                    best = length(v.pos - player_.position);
                    at = v.pos + Vec3{0, 0.9f, 0};
                    shoot = true;
                    found = true;
                }
            return found;
        }
        default:
            return false;
    }
}

// Before the frame: the weapon hand points at the target and fires when in range (a copy of the input).
void GameApp::botFireInput(float dt, InputFrame& in) {
    if (!botOn()) return;
    botPickT_ -= dt;
    if (botPickT_ <= 0) {
        botPickT_ = 0.5f;
        int best = -1;
        float bestD = 1e30f;
        for (size_t i = 0; i < objectives_.size(); ++i) {
            Vec3 at;
            bool shoot;
            if (!botTargetOf(int(i), at, shoot)) continue;
            const float d = length(Vec3{at.x - player_.position.x, 0, at.z - player_.position.z});
            if (d < bestD) { bestD = d; best = int(i); }
        }
        // (keep the current one unless another is much nearer: two Reapers 500 m off swapped places as nearest)
        Vec3 cur;
        bool curShoot;
        if (best != botObjective_ && botObjective_ >= 0 && size_t(botObjective_) < objectives_.size() && botTargetOf(botObjective_, cur, curShoot) &&
            bestD > 0.7f * length(Vec3{cur.x - player_.position.x, 0, cur.z - player_.position.z}))
            best = botObjective_;
        if (best != botObjective_) {
            botObjective_ = best;
            botPlanned_ = false;
            botReplans_ = 0;
            if (best >= 0) { Vec3 tp; bool ts; botTargetOf(best, tp, ts); LOGI("mission bot: objective %d (kind %d), %.0f m away at (%.0f %.0f %.0f)", best, objectives_[size_t(best)].kind, bestD, tp.x, tp.y, tp.z); }
        }
    }
    botEngaged_ = false;
    if (botObjective_ < 0) return;
    Vec3 at;
    bool shoot;
    if (!botTargetOf(botObjective_, at, shoot)) { botPickT_ = 0; return; }
    botTarget_ = at;
    // In range and in sight (from the eyes, over the terrain and the scenery): stop and fire.
    const float range = hero_ == Hero::Delphi ? 45.0f : 60.0f;
    const float d = length(at - player_.position);
    if (!shoot || d > range) { botSightless_ = 0; return; }
    const Vec3 eye = player_.position + Vec3{0, 1.6f, 0};
    Vec3 seeTo = at;
    {   // (a building: in sight up to its near side - the line to its middle ends inside its own walls or hull)
        const Objective& ob = objectives_[size_t(botObjective_)];
        if (ob.kind == 5 && ob.item >= 0 && size_t(ob.item) < destruct_.items.size()) {
            const float back = std::min(0.9f * d, 0.5f * destruct_.items[size_t(ob.item)].radius + 2.0f);
            seeTo = at - normalize(at - eye) * back;
        }
    }
    static const bool trace = std::getenv("GVR_BOT_TRACE") != nullptr;
    const bool los = d <= 6.0f || enemies_.lineOfSight(eye, seeTo);
    if (trace && int(time_ * 1.0) != int((time_ - dt) * 1.0))
        LOGI("bot trace: target %.1f m, in sight %d, swimming %d", d, int(los), int(player_.swimming()));
    if (!los) { botSightless_ += dt; return; }
    botSightless_ = 0;
    botEngaged_ = true;
    if (hero_ == Hero::Delphi && delphiBow()) delphiWeapon_ = -1;  // (the sword in hand: the bolt hand's trigger casts; with a bow it draws)
    // Point the hand that fires at it (Baz's gun hand; Delphi's bolt hand), from where it is.
    const int hand = hero_ == Hero::Delphi ? rig_.moveHand() : rig_.turnHand();
    Vec3 from = rig_.toWorld(in.hand[hand].aimPose).position;
    if (laserOn_ && length(laserFrom_ - from) < 1.5f * rig_.comfort.worldScale) from = laserFrom_;
    const Vec3 dir = rig_.worldFromTracking.orientation.conjugate().rotate(normalize(at - from));
    const float yaw = std::atan2(-dir.x, -dir.z), pitch = std::asin(std::max(-1.0f, std::min(1.0f, dir.y)));
    in.hand[hand].gripPose.orientation = in.hand[hand].aimPose.orientation =
        Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({1, 0, 0}, pitch);
    botFireT_ += dt;
    // Pulses: on 0.35 s, off 0.25 s (one-shot guns and Delphi's taps need the release; held guns barely notice).
    const bool pull = std::fmod(botFireT_, 0.6f) < (hero_ == Hero::Delphi ? 0.15f : 0.35f);
    in.hand[hand].trigger = pull ? 1.0f : 0.0f;
}

// Movement: along the planned path to the target, standing still while it fires.
void GameApp::botWalkInput(Vec2& move, float& viewYaw, bool& jump, float dt) {
    if (!botOn() || botObjective_ < 0 || reachTarget_ >= 0) return;
    const Vec3 p{player_.position.x, 0, player_.position.z};
    const Vec3 flat{botTarget_.x - p.x, 0, botTarget_.z - p.z};
    if (botEngaged_) {
        viewYaw = std::atan2(-flat.x, -flat.z);
        move = Vec2{0, 0};
        botStuckT_ = 0;
        return;
    }
    // A leg to the target's box (a place to reach: its own box), walked by the reach walker (it blocks where the
    // controller won't go and plans round; the jetpack up banks). Planned afresh when the target changes or has moved
    // 12 m; given up (no way): straight at it.
    const Objective& o = objectives_[size_t(botObjective_)];
    const Vec3 h{3, 3, 3};
    const Vec3 mn = o.kind == 1 ? o.boundsMin : botTarget_ - h, mx = o.kind == 1 ? o.boundsMax : botTarget_ + h;
    if (!botPlanned_ || length(botTarget_ - botPlanAt_) > 12.0f) {
        botPlanned_ = true;
        botPlanAt_ = botTarget_;
        reachReplans_ = 0;
        reachResets_ = 0;
        reachStuckT_ = 0;
        if (reachBlocked_.size()) std::fill(reachBlocked_.begin(), reachBlocked_.end(), uint8_t(0));
        if (!planReachBox(mn, mx, false, botObjective_)) reachPath_.clear();
    }
    // In range but out of sight for a while (a bunker over a ridge): Baz flies up and toward it until he sees it.
    if (hero_ == Hero::Mecc && player_.jetpackAvailable && botSightless_ > 6.0f) {
        viewYaw = std::atan2(-flat.x, -flat.z);
        move = Vec2{0, 0.6f};
        jump = player_.fuel() > 0.05f && !(player_.onGround && player_.fuel() < 0.9f);
        if (botSightless_ > 30.0f) botSightless_ = 0;  // (then the path again)
        return;
    }
    if (reachPath_.empty() || !walkBoxInput(mn, mx, botObjective_, move, viewYaw, jump, dt)) {
        viewYaw = std::atan2(-flat.x, -flat.z);
        move = Vec2{0, 1};
        jump = hero_ == Hero::Mecc && player_.jetpackAvailable && player_.fuel() > 0.2f && std::fmod(botStuckT_ += dt, 3.0f) < 1.5f;
    }
    if (++botLog_ % 720 == 0)
        LOGI("mission bot: at (%.0f %.0f %.0f), objective %d %.0f m away, %zu waypoints", player_.position.x, player_.position.y, player_.position.z,
             botObjective_, length(flat), reachPath_.size());
}

}  // namespace gvr
