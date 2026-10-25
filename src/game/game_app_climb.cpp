// GameApp, rework: climbing (first person, Kabuto and Delphi - the Meccaryn has his jetpack).
// Squeeze a grip with that hand on a steep face of the ground (the hand at or in the surface where
// it is steeper than you can walk): the hand holds on, and moving it moves you the other way - pull
// down to climb, reach up and grab again. Both hands can hold; let go of both and you drop (or stand,
// if you are over the top). Kabuto's long arms pull 1.5 times as far.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/terrain.h"

namespace gvr {

void GameApp::updateClimb(float dt, const InputFrame& in) {
    const bool can = playMode_ && firstPerson_ && !dying_ && !cutsceneActive() && assetWorld_ && !riding_ &&
                     (hero_ == Hero::Kabuto || hero_ == Hero::Delphi);
    static const bool trace = std::getenv("GVR_CLIMB_TRACE") != nullptr;
    const float S = rig_.comfort.worldScale;
    const float H = player_.height;
    const Quat rq = rig_.worldFromTracking.orientation;
    Vec3 move{0, 0, 0};
    int holding = 0;
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        const bool g = c.squeeze > 0.7f;
        const bool edge = g && !climbGripWas_[h];
        climbGripWas_[h] = g;
        if (!can || !c.gripPoseValid || c.squeeze < 0.35f) {
            if (climbHold_[h] && trace) LOGI("climb: hand %d lets go", h);
            climbHold_[h] = false;
            continue;
        }
        if (climbHold_[h]) {
            // The hand stays where it holds: the body moves by its tracked motion, reversed.
            const Vec3 d = c.gripPose.position - climbAnchor_[h];
            move = move - rq.rotate(d) * (S * (hero_ == Hero::Kabuto ? 1.5f : 1.0f));
            climbAnchor_[h] = c.gripPose.position;
            ++holding;
            continue;
        }
        if (!edge || (hero_ == Hero::Kabuto && (kbHold_ != HoldNone || kb_.eatT >= 0))) continue;
        // A steep face under the hand?
        const Vec3 p = rig_.toWorld(c.gripPose).position;
        float y0, yx, yz, yx2, yz2;
        const Terrain& terr = assetWorld_->terrain();
        const float e = std::max(0.5f, 0.06f * H);
        if (!terr.heightAt(p.x, p.z, y0) || !terr.heightAt(p.x + e, p.z, yx) || !terr.heightAt(p.x - e, p.z, yx2) ||
            !terr.heightAt(p.x, p.z + e, yz) || !terr.heightAt(p.x, p.z - e, yz2))
            continue;
        const float gx = (yx - yx2) / (2 * e), gz = (yz - yz2) / (2 * e);
        const float grade = std::sqrt(gx * gx + gz * gz);
        const bool atSurface = p.y < y0 + 0.08f * S;  // at or in it (a little above counts)
        if (!atSurface || grade < 0.9f) {
            if (trace && edge) LOGI("climb: hand %d - no hold (%.1f m above the ground, grade %.2f)", h, p.y - y0, grade);
            continue;
        }
        // Not when something to grab is right there (Kabuto's hand grab takes it).
        if (hero_ == Hero::Kabuto) {
            bool grabbable = false;
            for (const auto& en : enemies_.enemies)
                if (en.state != EnemyState::Dead && length(Vec3{en.pos.x - p.x, 0, en.pos.z - p.z}) < 0.24f * H) grabbable = true;
            for (const auto& v : villagers_.villagers)
                if (length(Vec3{v.pos.x - p.x, 0, v.pos.z - p.z}) < 0.24f * H) grabbable = true;
            if (grabbable) continue;
        }
        climbHold_[h] = true;
        climbAnchor_[h] = c.gripPose.position;
        haptics_.push_back({h, 0.6f, 0.08f});
        sfx2D(hero_ == Hero::Kabuto ? "ka_grab" : "sr_fswd1", 0.6f, 0.9f + 0.2f * frand());
        if (trace) LOGI("climb: hand %d takes hold (grade %.2f)", h, grade);
        ++holding;
    }
    if (holding > 0) {
        if (holding == 2) move = move * 0.5f;  // (both hands: their average)
        const float l = length(move);
        const float maxStep = 0.5f * H;  // a tracking glitch never flings him
        if (l > maxStep) move = move * (maxStep / l);
        player_.position = player_.position + move;
        if (!player_.climbing && trace) LOGI("climb: hanging on at (%.1f %.1f %.1f)", player_.position.x, player_.position.y, player_.position.z);
        player_.climbing = true;
        climbT_ += dt;
        if (l > 0.02f * S && int(climbT_ * 4) != int((climbT_ - dt) * 4))
            for (int h = 0; h < 2; ++h)
                if (climbHold_[h]) haptics_.push_back({h, 0.15f, 0.04f});  // a scrape as he hauls
    } else {
        if (player_.climbing && trace) LOGI("climb: off at (%.1f %.1f %.1f)", player_.position.x, player_.position.y, player_.position.z);
        player_.climbing = false;
        climbT_ = 0;
    }
}

}  // namespace gvr
