// Flares (2b's briefing: "You can throw homing missiles off your trail by using FLARES, found in
// the Shop"). Bought at a Shop (three at a time, up to six); the move-hand trigger throws one up and
// back off the jetpack (ME_FLARE). It burns bright for 5 s, drifting down; a SAM's homing missile
// within reach of a burning flare goes for the flare instead of the player and bursts there.
// How the original's flares fly and decoy is not decoded; this is ours.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"

namespace gvr {

namespace {
constexpr float kFlareLife = 5.0f;
constexpr float kDecoyRange = 120.0f;  // m: a missile this close to a flare takes it
}  // namespace

void GameApp::updateFlares(float dt, const InputFrame& in) {
    flareFlash_ = std::max(0.0f, flareFlash_ - dt * 2.0f);
    static bool hook = true;  // test hook: GVR_FLARES=n starts with n flares
    if (hook) { hook = false; if (const char* n = std::getenv("GVR_FLARES")) flares_ = std::max(0, std::min(kMaxFlares, std::atoi(n))); }
    // Throw one: the move hand's trigger (Meccaryn on foot; Delphi's trigger is her fire bolt).
    const bool trig = in.hand[rig_.moveHand()].trigger > 0.6f;
    const bool press = flareEdge_.pressed(trig);
    if (press && hero_ == Hero::Mecc && !riding_ && !mapOpen_ && playMode_ && !dying_) {
        if (flares_ > 0) {
            --flares_;
            flareFlash_ = 1.0f;
            const Vec3 back{std::sin(player_.yaw), 0, std::cos(player_.yaw)};
            Flare f;
            f.pos = player_.position + Vec3{0, 1.4f, 0} + back * 0.3f;
            f.vel = player_.velocity * 0.5f + back * 3.0f + Vec3{(frand() - 0.5f) * 2.0f, 7.0f, (frand() - 0.5f) * 2.0f};
            f.age = 0;
            flareList_.push_back(f);
            sfx2D("me_flare", 1.0f);
            haptics_.push_back({rig_.moveHand(), 0.5f, 0.08f});
            LOGI("Flare away (%d left)", flares_);
        } else {
            sfx2D("ph_selct", 0.4f, 0.6f);  // none left
        }
    }
    for (auto& f : flareList_) {
        f.age += dt;
        f.vel.y -= 4.0f * dt;                                  // floats down slowly
        f.vel = f.vel * std::max(0.0f, 1.0f - 0.8f * dt);      // air drag
        f.pos += f.vel * dt;
        float g;
        if (assetWorld_ && assetWorld_->terrain().heightAt(f.pos.x, f.pos.z, g) && f.pos.y < g + 0.1f) {
            f.pos.y = g + 0.1f;
            f.vel = Vec3{0, 0, 0};
        }
        // Burning: a white-hot core, a red-orange glow and a smoke trail.
        if (f.age < kFlareLife) {
            smokePuff(FxTex::Fire, f.pos, Vec3{0, 0.2f, 0}, 0.9f, 0.3f, 0.18f, 1.0f, 0.0f);
            if (frand() < 0.5f) smokePuff(FxTex::FireGreen, f.pos, Vec3{0, 0.1f, 0}, 0.35f, 0.1f, 0.12f, 1.0f, 0.0f);
            if (frand() < dt * 20.0f) smokePuff(FxTex::SmokeGrey, f.pos, Vec3{0, 0.4f, 0}, 0.3f, 1.6f, 2.0f, 0.4f, 0.3f);
        }
    }
    flareList_.erase(std::remove_if(flareList_.begin(), flareList_.end(), [](const Flare& f) { return f.age > kFlareLife; }),
                     flareList_.end());
}

bool GameApp::flareTarget(const Vec3& from, Vec3& out) const {
    float best = kDecoyRange;
    bool found = false;
    for (const auto& f : flareList_) {
        if (f.age > kFlareLife - 0.3f) continue;
        const float d = length(f.pos - from);
        if (d < best) { best = d; out = f.pos; found = true; }
    }
    return found;
}

}  // namespace gvr
