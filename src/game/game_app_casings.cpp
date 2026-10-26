// GameApp, rework: spent casings. The Meccaryn's machine gun, sniper gun and shotgun throw a casing out
// to the right with every shot (the original mc_machinegun_shell model - a cutscene prop - larger for
// the heavy guns); it tumbles, tinks on the ground, bounces and lies a few seconds. Our own design.
#include <algorithm>
#include <cmath>

#include "game/game_app.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
constexpr size_t kMaxCasings = 40;
}

void GameApp::ejectCasing(const Vec3& muzzle, const Vec3& fwd, float scale) {
    if (!assetWorld_) return;
    if (!casingModel_) casingModel_ = assetWorld_->models()->get("mc_machinegun_shell.gbs");
    if (!casingModel_) return;
    Vec3 right = cross(fwd, Vec3{0, 1, 0});
    right = length(right) > 1e-3f ? normalize(right) : Vec3{1, 0, 0};
    const Vec3 up = normalize(cross(right, fwd));
    if (casings_.size() >= kMaxCasings) casings_.erase(casings_.begin());
    Casing c;
    c.pos = muzzle - fwd * (0.35f + 0.1f * scale) + right * 0.06f + up * 0.03f;
    c.vel = right * (1.8f + 0.8f * frand()) + up * (1.4f + 0.8f * frand()) - fwd * (0.3f * frand()) + player_.velocity;
    c.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + up * 0.4f);
    c.spin = 14.0f + 10.0f * frand();
    c.angle = frand() * 6.28f;
    c.scale = scale;
    casings_.push_back(c);
}

void GameApp::updateCasings(float dt) {
    if (casingsWorld_ != assetWorld_.get()) { casingsWorld_ = assetWorld_.get(); casings_.clear(); casingModel_ = nullptr; }
    for (auto& c : casings_) {
        c.life += dt;
        if (c.resting) continue;
        c.vel.y -= 9.8f * dt;
        c.pos = c.pos + c.vel * dt;
        c.angle += c.spin * dt;
        if (c.pos.y < player_.seaLevel) { c.life = 1e9f; continue; }  // (into the water: gone)
        const float g = enemies_.groundHeight ? enemies_.groundHeight(c.pos.x, c.pos.z) + 0.015f * c.scale : c.pos.y - 1.0f;
        if (c.pos.y < g) {
            c.pos.y = g;
            if (c.vel.y < -1.0f && length(c.pos - player_.position) < 12.0f)
                sfx("ex_metls", c.pos, std::min(0.25f, 0.06f - c.vel.y * 0.03f) * c.scale, 2.2f + 0.4f * frand() - 0.3f * c.scale);  // a tink
            c.vel = Vec3{c.vel.x * 0.45f, -c.vel.y * 0.35f, c.vel.z * 0.45f};
            c.spin *= 0.5f;
            if (++c.bounces >= 3 || length(c.vel) < 0.4f) c.resting = true;
        }
    }
    casings_.erase(std::remove_if(casings_.begin(), casings_.end(), [](const Casing& c) { return c.life > 9.0f; }), casings_.end());
}

void GameApp::renderCasings() {
    if (!casingModel_) return;
    const Vec3 eye = renderer_.currentView().eyePosition;
    for (const auto& c : casings_) {
        if (length(c.pos - eye) > 25.0f) continue;
        drawModel(casingModel_, Mat4::translation(c.pos) * Mat4::rotation(Quat::fromAxisAngle(c.axis, c.angle)) *
                                    Mat4::scale({c.scale, c.scale, c.scale}),
                  false);
    }
}

}  // namespace gvr
