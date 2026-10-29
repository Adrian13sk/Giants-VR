// (2026-10-29) Test hook GVR_MISSION_BOT=1: plays a mission's objectives the way a player would, with the real
// controls - so a test can show a mission is finishable by hand, not only marked complete (GVR_AUTOCOMPLETE, the
// campaign run). story3_prison_6 had passed every test that way while no player could finish it (no lift).
// Each half second it picks the nearest objective left: a place to reach, a character or Reaper to kill, a building
// to bring down. It walks there along the reach planner's path (the jetpack up banks), and in range it stops, points
// the weapon hand at the target and fires: Baz holds the trigger (pulses for one-shot guns), Delphi throws fire
// bolts. Not for Kabuto (eating, growing and gates have their own tests) nor rescues, meat and races (their own
// autopilots: GVR_AUTO_RESCUE, GVR_SKI_AUTOPILOT).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"

namespace gvr {

namespace {
constexpr float kBotCell = 2.0f;
}

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
            at = destruct_.items[size_t(o.item)].base + Vec3{0, 0.4f * destruct_.items[size_t(o.item)].height, 0};
            shoot = true;
            return true;
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
            if (best >= 0) LOGI("mission bot: objective %d (kind %d), %.0f m away", best, objectives_[size_t(best)].kind, bestD);
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
    if (!shoot || d > range) return;
    const Vec3 eye = player_.position + Vec3{0, 1.6f, 0};
    if (d > 6.0f && !enemies_.lineOfSight(eye, at)) return;
    botEngaged_ = true;
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
    botReplanT_ -= dt;
    if (!botPlanned_ || botReplanT_ <= 0 || reachPath_.empty()) {
        botPlanned_ = true;
        botReplanT_ = 6.0f;  // (the target moves: plan again now and then)
        const Objective& o = objectives_[size_t(botObjective_)];
        const Vec3 h{3, 3, 3};
        Vec3 goal = botTarget_;
        // Stuck going for a target to shoot: a firing spot round it instead (in sight of it, on land), as a player
        // would look for one - a Raik on a ridge, Rippers over a cliff from the sea.
        if (o.kind != 1 && botReplans_ >= 6) {
            const float r = hero_ == Hero::Delphi ? 25.0f : 40.0f;
            float bestD = 1e30f;
            for (int k = 0; k < 24; ++k) {
                const float a = 6.2832f * float(k) / 24.0f + 0.37f * float(botReplans_ / 6);
                Vec3 c = botTarget_ + Vec3{std::cos(a) * r, 0, std::sin(a) * r};
                float g;
                if (!assetWorld_->terrain().heightAt(c.x, c.z, g) || g < player_.seaLevel + 0.3f) continue;
                c.y = g;
                if (!enemies_.lineOfSight(c + Vec3{0, 1.6f, 0}, botTarget_)) continue;
                const float dd = length(c - player_.position);
                if (dd < bestD) { bestD = dd; goal = c; }
            }
            if (bestD < 1e29f && (++botLog_ % 4) == 0) LOGI("mission bot: a firing spot %.0f m from the target", r);
        }
        if (o.kind == 1 ? !planReachBox(o.boundsMin, o.boundsMax, false, botObjective_) : !planReachBox(goal - h, goal + h, false, botObjective_)) {
            reachPath_.clear();
            // No way on foot: straight at it (the jetpack, swimming).
            viewYaw = std::atan2(-flat.x, -flat.z);
            move = Vec2{0, 1};
            jump = hero_ == Hero::Mecc && player_.jetpackAvailable && player_.fuel() > 0.2f && std::fmod(botStuckT_ += dt, 3.0f) < 1.5f;
            return;
        }
    }
    const float near = std::max(3.0f, 1.5f * player_.radius);
    size_t dropped = 0;
    while (reachPath_.size() > 1 && length(reachPath_.front() - p) < near) { reachPath_.erase(reachPath_.begin()); ++dropped; }
    botStuckT_ = dropped ? 0.0f : botStuckT_ + dt;
    if (botStuckT_ > 3.0f) {
        // Block the spot ahead and go round it.
        const Vec3 w = reachPath_.front();
        const int wx = int(std::lround((w.x - reachLo_.x) / kBotCell)), wz = int(std::lround((w.z - reachLo_.z) / kBotCell));
        if (wx >= 0 && wz >= 0 && wx < reachW_ && size_t(wz) * reachW_ + wx < reachBlocked_.size()) reachBlocked_[size_t(wz) * reachW_ + wx] = 1;
        botStuckT_ = 0;
        botPlanned_ = false;
        if (++botReplans_ % 20 == 0) std::fill(reachBlocked_.begin(), reachBlocked_.end(), uint8_t(0));
    }
    const Vec3 aim = reachPath_.empty() ? flat : reachPath_[std::min<size_t>(reachPath_.size() - 1, 1)] - p;
    viewYaw = std::atan2(-aim.x, -aim.z);
    move = Vec2{0, 1};
    jump = botStuckT_ > 0.6f && (hero_ == Hero::Mecc && player_.jetpackAvailable ? std::fmod(botStuckT_, 2.0f) < 1.3f : std::fmod(botStuckT_, 0.8f) < 0.25f);
    if (++botLog_ % 720 == 0)
        LOGI("mission bot: at (%.0f %.0f %.0f), objective %d %.0f m away, %zu waypoints", player_.position.x, player_.position.y, player_.position.z,
             botObjective_, length(flat), reachPath_.size());
}

}  // namespace gvr
