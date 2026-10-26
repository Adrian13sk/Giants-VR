// GameApp, rework: the sea monster. xx_seamonster.gzp holds a 30 m sea serpent (RM_seamonster,
// with swim and rearing "fire_turn" clips and its roars and splashes, SE_*) that no level we load
// places. Ours: now and then, out at sea in front of you, it surfaces, swims across your view,
// rears up out of the water with a roar, crashes back and dives. A spectacle, not a fight: it
// never comes ashore. Drawn under the sea's surface pass (AssetWorld::underwater) so its body
// below the water shows through it.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/game_app.h"
#include "scene/asset_world.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
enum { kHidden, kSurface, kSwim, kRear, kSwim2, kDive };
constexpr float kSurfaceT = 2.5f, kSwimT = 7.0f, kSwim2T = 4.0f, kDiveT = 3.0f;
constexpr float kDepthSwim = 1.6f;  // its origin below the surface while swimming (the back and fins out)
}  // namespace

void GameApp::initSeaMonster(AssetWorld* w, ModelCache* mc) {
    seaMon_ = SeaMonster{};
    const char* e = std::getenv("GVR_SEAMONSTER");
    seaMon_.timer = e ? float(std::atof(e)) : 80.0f + 80.0f * frand();
    if (!w || !mc || std::getenv("GVR_NO_SEAMONSTER")) return;
    if (seaMonModel_ && seaMonCache_ == mc) return;
    seaMonModel_.reset();
    seaMonCache_ = mc;
    auto m = std::make_unique<SkinnedModel>();
    if (!m->load(w->fs(), *mc, renderer_, "RM_seamonster_L1", "RM_swim.anm")) { LOGW("Sea monster: model unavailable"); return; }
    seaMonRear_ = std::max(0, m->addAnimation(w->fs(), "Rm_fire_turn.anm"));
    Vec3 mn, mx;
    m->boundsAt(0, 0.0f, mn, mx);
    LOGI("Sea monster: %.0f x %.0f x %.0f m, rearing clip %.1f s", mx.x - mn.x, mx.y - mn.y, mx.z - mn.z, m->animDuration(seaMonRear_));
    m->pose(renderer_, 0.0f);
    seaMonModel_ = std::move(m);
}

void GameApp::updateSeaMonster(float dt) {
    SeaMonster& s = seaMon_;
    if (!seaMonModel_ || !assetWorld_ || player_.seaLevel < -1e8f) return;
    const float sea = player_.seaLevel;
    const Terrain& terr = assetWorld_->terrain();
    auto depth = [&](float x, float z) {
        float g;
        return terr.heightAt(x, z, g) ? sea - g : 50.0f;  // (off the map: open sea)
    };
    auto roar = [&](const char* name, float vol) {
        // Heard across the bay: the mixer's 1/d roll-off undone for most of the distance.
        const float d = std::max(2.0f, length(s.pos - player_.position));
        sfx(name, s.pos + Vec3{0, 6.0f, 0}, std::min(30.0f, vol * d / 6.0f), 0.9f + 0.1f * frand());
    };
    s.t += dt;
    s.phaseT += dt;
    if (s.phase == kHidden) {
        if (!playMode_ || cutsceneActive() || title_) return;
        s.timer -= dt;
        if (s.timer > 0) return;
        s.timer = 20.0f;  // (no room this time: look again soon)
        // A stretch of deep water ahead of the eyes, 35-65 m out, long enough to swim across.
        const Pose head = rig_.toWorld(lastInput_.head);
        Vec3 f = head.orientation.rotate(Vec3{0, 0, -1});
        f.y = 0;
        if (length(f) < 1e-3f) return;
        f = normalize(f);
        for (int tries = 0; tries < 24; ++tries) {
            const float a = (frand() - 0.5f) * 1.6f, d = 35.0f + 30.0f * frand();
            const Vec3 dir{f.x * std::cos(a) - f.z * std::sin(a), 0, f.x * std::sin(a) + f.z * std::cos(a)};
            const Vec3 c = head.position + dir * d;
            const Vec3 across = Vec3{-dir.z, 0, dir.x} * (frand() < 0.5f ? 1.0f : -1.0f);
            bool ok = true;
            for (int k = -2; k <= 3 && ok; ++k) {
                const Vec3 p = c + across * (float(k) * 12.0f);
                ok = depth(p.x, p.z) > 7.0f;
            }
            if (!ok) continue;
            s.pos = Vec3{c.x - across.x * 20.0f, sea - 10.0f, c.z - across.z * 20.0f};
            s.dir = across;
            s.yaw = std::atan2(-across.x, -across.z);
            s.phase = kSurface;
            s.phaseT = 0;
            LOGI("Sea monster: surfaces %.0f m out", d);
            waterSplash(Vec3{s.pos.x, sea, s.pos.z}, 3.0f, 0);
            roar("se_first", 1.0f);
            return;
        }
        return;
    }
    // Moving along its line (slower while rearing); the height by phase.
    const float speed = s.phase == kRear ? 1.5f : 7.0f;
    s.pos = s.pos + s.dir * (speed * dt);
    float y = sea - kDepthSwim;
    if (s.phase == kSurface) {
        const float u = std::min(1.0f, s.phaseT / kSurfaceT);
        y = sea - 10.0f + (10.0f - kDepthSwim) * (u * u * (3.0f - 2.0f * u));
        if (s.phaseT > kSurfaceT) { s.phase = kSwim; s.phaseT = 0; }
    } else if (s.phase == kSwim) {
        if (s.phaseT > kSwimT) {
            s.phase = kRear;
            s.phaseT = 0;
            s.splashes = 0;
            roar("se_roar1", 1.2f);
            LOGI("Sea monster: rears up");
        }
    } else if (s.phase == kRear) {
        const float len = seaMonModel_->animDuration(seaMonRear_);
        // Water thrown up as it rises, and when it crashes back.
        const float marks[3] = {0.15f, 0.62f, 0.85f};
        if (s.splashes < 3 && s.phaseT > marks[s.splashes] * len) {
            waterSplash(Vec3{s.pos.x, sea, s.pos.z} + s.dir * (4.0f * float(s.splashes)), 3.5f, 0);
            if (s.splashes == 1) roar("se_splsh", 1.4f);
            ++s.splashes;
        }
        if (s.phaseT > len) { s.phase = kSwim2; s.phaseT = 0; }
    } else if (s.phase == kSwim2) {
        if (s.phaseT > kSwim2T) {
            s.phase = kDive;
            s.phaseT = 0;
            roar("se_firnd", 0.8f);
        }
    } else if (s.phase == kDive) {
        const float u = std::min(1.0f, s.phaseT / kDiveT);
        y = sea - kDepthSwim - 12.0f * u * u;
        if (s.phaseT > 0.4f && s.splashes < 4) { waterSplash(Vec3{s.pos.x, sea, s.pos.z}, 2.5f, 0); s.splashes = 4; }
        if (s.phaseT > kDiveT) {
            s.phase = kHidden;
            s.timer = std::getenv("GVR_SEAMONSTER") ? 8.0f : 180.0f + 150.0f * frand();
            LOGI("Sea monster: dives");
        }
    }
    s.pos.y = y;
    // Pose: swimming, or rearing up.
    if (s.phase == kRear) seaMonModel_->poseBlend(renderer_, seaMonRear_, s.phaseT, seaMonRear_, s.phaseT, 0.0f);
    else seaMonModel_->pose(renderer_, s.t);
}

void GameApp::renderSeaMonster() {
    if (!seaMonModel_ || seaMon_.phase == kHidden || renderer_.inReflectionPass()) return;
    seaMonModel_->render(renderer_, Mat4::translation(seaMon_.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, seaMon_.yaw)));
}

}  // namespace gvr
