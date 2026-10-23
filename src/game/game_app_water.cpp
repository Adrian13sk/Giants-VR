// Water effects beyond the rings: splashes that rise and fall (the original efx_splash crown, whose
// clip only carries debris reference points, animated here: it grows, rises and sinks back) with
// drops (b_drip) and spray (EFX_splash), shots landing in the water (bi_watr1), the water parting
// round the player's legs while wading or swimming, and the piranhas' frenzy - blood spurting from
// the water, chomping and thrashing - when they eat the player or a Smartie.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "scene/asset_world.h"

namespace gvr {

void GameApp::waterSplash(const Vec3& at, float size, int sound) {
    const float sea = player_.seaLevel;
    const Vec3 s{at.x, sea + 0.02f, at.z};
    if (waterFx_.size() < 60) waterFx_.push_back({s, 0.0f, 0, size});
    if (waterFx_.size() < 60) waterFx_.push_back({Vec3{at.x, sea + 0.03f, at.z}, 0.0f, 1, size * 1.8f});
    // Drops thrown up and out, falling back.
    const int drops = int(4 + 10 * std::min(2.0f, size));
    for (int i = 0; i < drops; ++i) {
        const float a = frand() * 6.28f, r = frand();
        Particle p;
        p.pos = s + Vec3{std::cos(a), 0, std::sin(a)} * (0.2f * size * r);
        p.vel = Vec3{std::cos(a) * (0.5f + 1.5f * r), 2.5f + 3.0f * frand(), std::sin(a) * (0.5f + 1.5f * r)} * std::sqrt(std::max(0.3f, size));
        p.size0 = p.size1 = (0.07f + 0.08f * frand()) * std::max(0.6f, size);
        p.life = 0.6f + 0.5f * frand();
        p.alpha = 0.9f;
        p.rise = -11.0f;
        p.drag = 0.2f;
        p.tex = FxTex::Drip;
        p.additive = true;
        fx_.emit(p);
    }
    // A puff of spray.
    for (int i = 0; i < 2 + int(size * 2); ++i)
        smokePuff(FxTex::Spray, s + Vec3{(frand() - 0.5f) * 0.4f * size, 0.2f * size, (frand() - 0.5f) * 0.4f * size}, Vec3{0, 1.2f + frand(), 0},
                  0.4f * size, 1.4f * size, 0.6f, 0.7f, -1.5f);
    if (sound == 1) sfx("bi_watr1", s, 0.6f, 0.9f + 0.25f * frand());
    else if (sound == 2) sfx("se_splsh", s, 0.9f, 0.95f + 0.1f * frand());
}

bool GameApp::shotIntoWater(const Vec3& p, const Vec3& dir) {
    // A shot whose end is under the surface crossed it on the way: the splash is where it went in.
    const float sea = player_.seaLevel;
    if (p.y > sea + 0.05f || dir.y > -0.01f) return false;
    const float back = (p.y - sea) / dir.y;  // >= 0: distance back along the shot to the surface
    if (back > 60.0f) return false;
    waterSplash(p - dir * back, 0.45f, 1);
    return true;
}

void GameApp::piranhaBite(const Vec3& at, float size, bool loud) {
    // The frenzy: blood spurting up out of the water, the water churned white, chomping.
    const float sea = player_.seaLevel;
    const Vec3 s{at.x + (frand() - 0.5f) * 0.6f, sea + 0.1f, at.z + (frand() - 0.5f) * 0.6f};
    blood_.burst(s, Vec3{(frand() - 0.5f) * 0.6f, 1.0f, (frand() - 0.5f) * 0.6f}, int(18 * size) + 8, 5.0f * std::sqrt(size), 0.1f * size);
    for (int k = 0; k < 2; ++k) bloodInWater(at, 0.6f + 0.6f * size);
    waterSplash(s, 0.5f * size, 0);
    sfx(frand() < 0.5f ? "ka_bite1" : "ka_bite2", s, loud ? 0.9f : 0.7f, 1.5f + 0.4f * frand());  // small jaws: pitched up
    if (frand() < 0.6f) sfx(frand() < 0.5f ? "ex_bdys1" : "ex_bdys2", s, loud ? 0.8f : 0.6f, 1.1f + 0.3f * frand());
    if (frand() < 0.4f) sfx("bi_watr1", s, 0.7f, 0.8f + 0.3f * frand());
}

void GameApp::updateWaterFlow(float dt) {
    // The water parting round the legs: a bow wave of spray ahead, drops kicked up, rings spreading
    // (rings: updateCombat); also when standing, a slow lapping ring.
    if (!playMode_ || dying_) return;
    const float sea = player_.seaLevel;
    static bool findSea = std::getenv("GVR_FIND_SEA") != nullptr;  // test hook: logs the nearest shallows and deep water
    if (findSea && enemies_.groundHeight) {
        findSea = false;
        float bestS = 1e9f, bestD = 1e9f;
        Vec3 s0, d0;
        for (float dz = -300; dz <= 300; dz += 4)
            for (float dx = -300; dx <= 300; dx += 4) {
                const Vec3 p{player_.position.x + dx, 0, player_.position.z + dz};
                float gh = enemies_.groundHeight(p.x, p.z);
                if (assetWorld_ && !assetWorld_->terrain().heightAt(p.x, p.z, gh)) continue;
                const float dd = dx * dx + dz * dz;
                if (gh < sea - 0.3f && gh > sea - 0.7f && dd < bestS) { bestS = dd; s0 = p; }
                if (gh < sea - 3.0f && dd < bestD) { bestD = dd; d0 = p; }
            }
        LOGI("Shallows at game (%.0f %.0f), deep water at game (%.0f %.0f)", -s0.x / 0.2f, s0.z / 0.2f, -d0.x / 0.2f, d0.z / 0.2f);
    }
    const float g = enemies_.groundHeight ? enemies_.groundHeight(player_.position.x, player_.position.z) : 0.0f;
    const bool swim = player_.swimming();
    const bool wading = !swim && player_.onGround && player_.position.y < sea - 0.08f;
    if (!swim && !wading) { flowT_ = 0; return; }
    const float depth = std::min(1.2f, sea - std::max(g, player_.position.y));
    const float v = player_.horizontalSpeed();
    flowT_ -= dt;
    if (flowT_ > 0) return;
    const bool moving = v > 0.4f;
    flowT_ = moving ? 0.07f : 0.6f;
    const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
    const Vec3 fwd = body.rotate(Vec3{0, 0, -1}), side = body.rotate(Vec3{1, 0, 0});
    const Vec3 vel{player_.velocity.x, 0, player_.velocity.z};
    const Vec3 dir = v > 0.1f ? vel * (1.0f / v) : fwd;
    if (!moving) {
        waterFx_.push_back({Vec3{player_.position.x, sea + 0.03f, player_.position.z}, 0.0f, 1, 0.9f});
        return;
    }
    const float k = std::min(1.0f, v / 6.0f);
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        // A curl of spray off each shin, pushed out to the side and ahead.
        const Vec3 at = Vec3{player_.position.x, sea + 0.05f, player_.position.z} + dir * 0.3f + side * (0.18f * float(sgn));
        const Vec3 out = side * (float(sgn) * (0.8f + 1.5f * k)) + dir * (v * 0.5f) + Vec3{0, 0.6f + 1.2f * k, 0};
        smokePuff(FxTex::Spray, at, out, 0.1f + 0.08f * k, 0.3f + 0.3f * k, 0.4f, 0.3f + 0.2f * k, -2.0f);
    }
    // Drops kicked up (more running, and from the depth of the water).
    if (frand() < 0.5f + 0.5f * k) {
        Particle p;
        p.pos = Vec3{player_.position.x, sea + 0.08f, player_.position.z} + dir * 0.25f + side * ((frand() - 0.5f) * 0.5f);
        p.vel = dir * (v * 0.6f) + side * ((frand() - 0.5f) * 2.0f) + Vec3{0, 1.5f + 2.5f * k * frand() + depth, 0};
        p.size0 = p.size1 = 0.06f + 0.06f * frand();
        p.life = 0.5f + 0.3f * frand();
        p.alpha = 0.85f;
        p.rise = -11.0f;
        p.tex = FxTex::Drip;
        p.additive = true;
        fx_.emit(p);
    }
    // Rings behind, spreading as the wake.
    flowRingT_ -= 0.05f;
    if (flowRingT_ <= 0 && waterFx_.size() < 56) {
        flowRingT_ = 0.22f;
        waterFx_.push_back({Vec3{player_.position.x, sea + 0.03f, player_.position.z} - dir * 0.2f, 0.0f, 1, 1.0f + 0.4f * k});
    }
    // Splashing steps when running through the shallows.
    if (wading && v > 3.0f && frand() < 0.15f) sfx("bi_watr1", player_.position, 0.35f, 1.1f + 0.3f * frand());
}

}  // namespace gvr
