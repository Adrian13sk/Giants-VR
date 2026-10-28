// GameApp, rework: spent casings. The Meccaryn's machine gun, sniper gun and shotgun throw a casing out
// to the right with every shot (the original mc_machinegun_shell model - a cutscene prop - larger for
// the heavy guns); it tumbles, tinks on the ground, bounces and lies a few seconds. Our own design.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"

#include "game/arm_ik.h"
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

void GameApp::stickArrow(int enemy, const Vec3& at, const Vec3& dir) {
    if (stuckArrows_.size() >= 30) stuckArrows_.erase(stuckArrows_.begin());
    StuckArrow a;
    a.dir = normalize(dir);
    a.pos = at + a.dir * 0.12f;  // (its head in a little)
    if (enemy >= 0 && size_t(enemy) < enemies_.enemies.size()) {
        const Enemy& e = enemies_.enemies[size_t(enemy)];
        const Quat inv = Quat::fromAxisAngle({0, 1, 0}, -e.yaw);
        a.uid = e.uid;
        a.local = inv.rotate(a.pos - e.pos);
        a.localDir = inv.rotate(a.dir);
    }
    // (2026-10-28) An arrow from a bow that counts its arrows can be taken back (walk over it or pick it up).
    // (The bow is the one the arrow was shot from - its tag - not the one in hand when it lands; a Reaper gun's
    // bolts and the fan's side arrows carry none.)
    const int tag = weapon_.hitTag();
    if (hero_ == Hero::Delphi && tag >= 0 && tag < kNumBows && bows_[tag].ammo >= 0) a.bow = tag;
    stuckArrows_.push_back(a);
    static const bool trace = std::getenv("GVR_HIT_TRACE") != nullptr;
    if (trace) LOGI("arrow stuck at (%.2f %.2f %.2f)%s", a.pos.x, a.pos.y, a.pos.z, a.uid ? " in a Reaper" : "");
}

void GameApp::updateCasings(float dt) {
    if (casingsWorld_ != assetWorld_.get()) { casingsWorld_ = assetWorld_.get(); casings_.clear(); casingModel_ = nullptr; stuckArrows_.clear(); }
    // Arrows in a Reaper ride on him (standing, falling, lying); a ragdoll or a torn-up body drops them.
    for (auto& a : stuckArrows_) {
        a.age += dt;
        if (!a.uid) continue;
        const Enemy* e = nullptr;
        for (const auto& en : enemies_.enemies)
            if (en.uid == a.uid) e = &en;
        if (!e || e->ragdoll || e->gibbed || e->remove) { a.age = 1e9f; continue; }
        const Quat r = Quat::fromAxisAngle({0, 1, 0}, e->yaw);
        a.pos = e->pos + Vec3{0, e->sink * -1.0f, 0} + r.rotate(a.local);
        a.dir = r.rotate(a.localDir);
    }
    // Taking arrows back (2026-10-28): Delphi's hand within 15 cm of one, or her feet within 0.9 m of one in the
    // ground - not one still in a living Reaper. Its bow gets it back, up to the bow's most.
    if (hero_ == Hero::Delphi && playMode_ && !dying_) {
        Vec3 hands[2];
        int nh = 0;
        for (int h = 0; h < 2; ++h)
            if (lastInput_.hand[h].gripPoseValid) hands[nh++] = rig_.toWorld(lastInput_.hand[h].gripPose).position;
        for (auto& a : stuckArrows_) {
            if (a.bow < 0 || a.bow >= kNumBows || a.age > 1e8f || !bows_[a.bow].have || bows_[a.bow].ammo < 0) continue;
            if (a.uid) {
                bool alive = false;
                for (const auto& en : enemies_.enemies) alive = alive || (en.uid == a.uid && en.state != EnemyState::Dead);
                if (alive) continue;
            }
            bool near = length(Vec3{a.pos.x - player_.position.x, 0, a.pos.z - player_.position.z}) < 0.9f &&
                        std::fabs(a.pos.y - player_.position.y) < 1.0f;
            for (int k = 0; k < nh && !near; ++k) near = length(hands[k] - a.pos) < 0.15f;
            if (!near || (bows_[a.bow].maxAmmo >= 0 && bows_[a.bow].ammo >= bows_[a.bow].maxAmmo)) continue;
            ++bows_[a.bow].ammo;
            a.age = 1e9f;  // (gone)
            sfx2D("ph_selct", 0.5f, 1.5f);
            haptics_.push_back({kLeft, 0.25f, 0.03f});
            LOGI("Arrow taken back: %s %d", bows_[a.bow].name, bows_[a.bow].ammo);
        }
    }
    stuckArrows_.erase(std::remove_if(stuckArrows_.begin(), stuckArrows_.end(), [](const StuckArrow& a) { return a.age > 20.0f; }), stuckArrows_.end());
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
    const Vec3 eye = renderer_.currentView().eyePosition;
    (void)eye;
    if (!casingModel_) return;
    for (const auto& c : casings_) {
        if (length(c.pos - eye) > 25.0f) continue;
        drawModel(casingModel_, Mat4::translation(c.pos) * Mat4::rotation(Quat::fromAxisAngle(c.axis, c.angle)) *
                                    Mat4::scale({c.scale, c.scale, c.scale}),
                  false);
    }
}

}  // namespace gvr
