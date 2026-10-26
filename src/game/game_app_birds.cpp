// GameApp, rework: ambient birds. The original ships xx_birds.gzp - the Flak (a small dark bird
// with peck / hop / fly / glide / land clips), the Lobird (a big glider) and a feather - but no
// level we load places them. Ours: Flak flocks peck about on open ground near the player and take
// off together when the player comes close, a gun goes off nearby, something blows up or a Reaper
// walks up; they climb away and are gone past the fog. Two or three Lobirds circle high over the
// player's area. A shot (or a thrown stone) through a bird kills it: it tumbles down in a puff of
// feathers (Feather1.gbs, drifting down) with the original's crow death squawk (CR_DEAT*).
// Our own behaviour on the original art and sounds.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "core/log.h"
#include "game/game_app.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr int kMaxBirds = 20;
constexpr int kLobirds = 3;
enum { kPeck1, kPeck2, kHop, kFly, kGlide, kLand, kLoFly };
enum { kPecking, kHopping, kFlying, kFalling, kDead };
Vec3 fwdOf(float yaw) { return Vec3{-std::sin(yaw), 0, -std::cos(yaw)}; }
}  // namespace

void GameApp::initBirds(AssetWorld* w, ModelCache* mc) {
    birds_.clear();
    birdModels_.clear();
    feathers_.clear();
    birdsReady_ = false;
    flockTimer_ = 6.0f;
    nextFlock_ = 0;
    birdShots_ = weapon_.shotsFired();
    if (!w || !w->hasTerrain() || !mc || std::getenv("GVR_NO_BIRDS")) return;
    // Probe the two skins once (bounds -> how far the origin is above the feet).
    static const char* flakClips[6] = {"Flak_peck_1.anm", "flak_peck_2.anm", "flak_hop.anm", "flak_fly.anm", "Flak_fly_glide.anm", "flak_land.anm"};
    for (int kind = 0; kind < 2; ++kind) {
        SkinnedModel probe;
        if (!probe.load(w->fs(), *mc, renderer_, kind == 0 ? "Flak_l0" : "Lobird_l0", kind == 0 ? flakClips[0] : "Lobird_fly.anm")) {
            LOGW("Birds: %s unavailable", kind == 0 ? "Flak_l0" : "Lobird_l0");
            return;
        }
        Vec3 mn, mx;
        probe.boundsAt(0, 0.0f, mn, mx);
        birdLift_[kind] = -mn.y;
        LOGI("Birds: %s size %.2f x %.2f x %.2f m", kind == 0 ? "Flak" : "Lobird", mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
    }
    featherModel_ = mc->get("Feather1.gbs", "Feather1_float.anm", true);
    // (the feather art is 0.58 m across - drawn at 0.45 for the Flak)
    if (featherModel_) LOGI("Birds: feather %.2f m", length(featherModel_->boundsMax - featherModel_->boundsMin));
    // The model pool: every bird has its own skin (each is posed on its own).
    for (int i = 0; i < kMaxBirds; ++i) {
        const bool lo = i < kLobirds;
        auto m = std::make_unique<SkinnedModel>();
        if (!m->load(w->fs(), *mc, renderer_, lo ? "Lobird_l0" : "Flak_l0", lo ? "Lobird_fly.anm" : flakClips[0])) return;
        if (lo) birdClip_[kLoFly] = 0;
        else
            for (int c = 1; c < 6; ++c) {
                const int id = m->addAnimation(w->fs(), flakClips[c]);
                birdClip_[c] = std::max(0, id);
            }
        m->pose(renderer_, 0.0f);
        birdModels_.push_back(std::move(m));
    }
    birdsReady_ = true;
    // The Lobirds: high circles round a centre that drifts after the player.
    for (int i = 0; i < kLobirds; ++i) {
        Bird b;
        b.kind = 1;
        b.state = kFlying;
        b.slot = i;
        b.centre = player_.position;
        b.circleR = 55.0f + 35.0f * frand();  // (a Lobird is huge: 17 m across the wings)
        b.circleH = 60.0f + 30.0f * frand();
        b.circleAng = float(i) * 2.0944f + frand() * 0.8f;
        b.circleSpeed = (frand() < 0.5f ? -1.0f : 1.0f) * (9.0f + 4.0f * frand()) / b.circleR;
        b.pos = b.centre + Vec3{std::cos(b.circleAng) * b.circleR, b.circleH, std::sin(b.circleAng) * b.circleR};
        b.t = frand() * 3.0f;
        b.clipLen = std::max(0.1f, birdModels_[size_t(i)]->animDuration(0));
        birds_.push_back(b);
    }
    LOGI("Birds: ready (%d Lobirds, Flak pool %d)", kLobirds, kMaxBirds - kLobirds);
}

bool GameApp::spawnFlock(const Vec3& near, bool test) {
    if (!birdsReady_ || !assetWorld_) return false;
    std::vector<int> free;
    for (int s = kLobirds; s < kMaxBirds; ++s) {
        bool used = false;
        for (const Bird& b : birds_) used |= b.slot == s && !b.gone;
        if (!used) free.push_back(s);
    }
    if (free.size() < 4) return false;
    const Terrain& t = assetWorld_->terrain();
    auto land = [&](float x, float z, float& y) {
        float a, c;
        if (!t.heightAt(x, z, y) || y < player_.seaLevel + 0.4f) return false;
        if (!t.heightAt(x + 1.5f, z, a) || !t.heightAt(x, z + 1.5f, c)) return false;
        return std::fabs(a - y) < 0.8f && std::fabs(c - y) < 0.8f;
    };
    float cy;
    if (!land(near.x, near.z, cy)) return false;
    const int n = std::min(int(free.size()), 4 + int(frand() * 4.0f));
    const int flock = nextFlock_++;
    int made = 0;
    for (int k = 0; k < n; ++k) {
        Vec3 p = near + Vec3{(frand() - 0.5f) * 5.0f, 0, (frand() - 0.5f) * 5.0f};
        if (!land(p.x, p.z, p.y)) continue;
        Bird b;
        b.kind = 0;
        b.state = kPecking;
        b.slot = free[size_t(made)];
        b.flock = flock;
        b.pos = p;
        b.yaw = frand() * 6.2832f;
        b.clip = frand() < 0.5f ? kPeck1 : kPeck2;
        b.t = frand() * 2.0f;
        b.clipLen = std::max(0.1f, birdModels_[size_t(b.slot)]->animDuration(birdClip_[b.clip]));
        auto it = std::find_if(birds_.begin(), birds_.end(), [](const Bird& o) { return o.gone; });
        if (it != birds_.end()) *it = b;
        else birds_.push_back(b);
        ++made;
    }
    if (made) LOGI("Birds: a flock of %d Flak lands at (%.1f %.1f %.1f)%s", made, near.x, cy, near.z, test ? " (test)" : "");
    return made > 0;
}

void GameApp::killBird(Bird& b, const Vec3& dir) {
    if (b.state == kFalling || b.state == kDead || b.gone) return;
    LOGI("Birds: a %s is shot down at (%.1f %.1f %.1f)", b.kind == 0 ? "Flak" : "Lobird", b.pos.x, b.pos.y, b.pos.z);
    b.state = kFalling;
    b.stateT = 0;
    b.vel = Vec3{b.vel.x * 0.4f + dir.x * 3.0f, std::max(1.5f, b.vel.y * 0.3f + 1.5f), b.vel.z * 0.4f + dir.z * 3.0f};
    b.bank = 0;
    b.clip = b.kind == 0 ? kGlide : kLoFly;
    sfx(frand() < 0.5f ? "cr_deat1" : "cr_deat2", b.pos, b.kind == 1 ? 1.0f : 0.8f, b.kind == 1 ? 0.8f : 1.15f);
    const int nf = b.kind == 1 ? 12 : 8;
    for (int k = 0; k < nf && feathers_.size() < 96; ++k) {
        Feather f;
        f.pos = b.pos + Vec3{0, birdLift_[b.kind] * 0.5f, 0};
        f.vel = Vec3{(frand() - 0.5f) * 3.0f, frand() * 2.0f, (frand() - 0.5f) * 3.0f} + dir * 1.5f;
        f.yaw = frand() * 6.2832f;
        f.spin = (frand() - 0.5f) * 6.0f;
        f.sway = frand() * 6.2832f;
        f.life = 5.0f + frand() * 4.0f;
        feathers_.push_back(f);
    }
    // The rest of its flock (on the ground) bolts.
    for (Bird& o : birds_)
        if (!o.gone && o.flock == b.flock && o.kind == 0 && (o.state == kPecking || o.state == kHopping)) {
            o.state = kFlying;
            o.stateT = -0.25f * frand();
            o.centre = b.pos;
        }
}

bool GameApp::birdHitBy(const Vec3& a, const Vec3& bEnd, float radius, const Vec3& dir) {
    if (!birdsReady_) return false;
    const Vec3 seg = bEnd - a;
    const float len2 = dot(seg, seg);
    for (Bird& b : birds_) {
        if (b.gone || b.state == kFalling || b.state == kDead) continue;
        const Vec3 c = b.pos + Vec3{0, birdLift_[b.kind] * 0.6f, 0};
        const float u = len2 > 1e-8f ? std::max(0.0f, std::min(1.0f, dot(c - a, seg) / len2)) : 0.0f;
        const float r = radius + (b.kind == 1 ? 4.0f : 0.3f);
        if (length(a + seg * u - c) < r) {
            killBird(b, dir);
            return true;
        }
    }
    return false;
}

void GameApp::updateBirds(float dt) {
    if (!birdsReady_ || !assetWorld_) return;
    const Terrain& terr = assetWorld_->terrain();
    auto ground = [&](float x, float z) {
        float y;
        return terr.heightAt(x, z, y) ? y : -1e9f;
    };
    const float big = std::max(1.0f, player_.height / 1.8f);  // (Kabuto scares them from much further)
    const Vec3 me = player_.position;
    if (nextFlock_ == 0)
        if (const char* e = std::getenv("GVR_BIRDS_TEST")) {  // (tests: a flock this far ahead of the player, at once)
            const float d = std::max(4.0f, float(std::atof(e)));
            if (!spawnFlock(me + fwdOf(player_.yaw) * d, true)) nextFlock_ = 1;
            for (Bird& b : birds_)
                if (b.kind == 1) b.centre = me;
        }
    // What spooks a flock this frame: the player near, a gun fired near, a blast, a Reaper.
    std::vector<Vec3> scares;
    for (const auto& bl : blasts_)
        if (bl.age < 0.05f) scares.push_back(bl.pos);
    const bool fired = weapon_.shotsFired() != birdShots_;
    birdShots_ = weapon_.shotsFired();
    // Shots through birds; and shots that strike the ground beside a pecking bird.
    for (const Projectile& p : weapon_.projectiles()) {
        const float sp = length(p.vel);
        if (sp < 1.0f) continue;
        birdHitBy(p.pos - p.vel * dt, p.pos, 0.05f, p.vel * (1.0f / sp));
    }
    for (const WeaponEffect& e : weapon_.effects())
        if (e.kind == WeaponEffect::Impact && e.age <= dt) birdHitBy(e.pos, e.pos + e.dir * 0.3f, 0.25f, Vec3{0, 1, 0} - e.dir);

    // New flocks now and then, out of sight to the side or behind, on open land.
    flockTimer_ -= dt;
    if (flockTimer_ <= 0 && playMode_) {
        flockTimer_ = 20.0f + 15.0f * frand();
        int live = 0;
        for (const Bird& b : birds_) live += !b.gone && b.kind == 0 && b.state <= kHopping;
        if (live < 6 && !std::getenv("GVR_BIRDS_TEST"))
            for (int tries = 0; tries < 10; ++tries) {
                const float yaw = player_.yaw + 1.6f + frand() * 3.08f;  // (the half behind the body)
                const Vec3 at = me + fwdOf(yaw) * ((16.0f + 12.0f * frand()) * big);
                if (spawnFlock(at, false)) break;
            }
    }

    for (Bird& b : birds_) {
        if (b.gone) continue;
        b.t += dt;
        b.stateT += dt;
        if (b.kind == 1) {
            // Lobird: a slow wide circle; its centre follows the player at walking pace.
            if (b.state == kFlying) {
                if (length(Vec3{me.x - b.centre.x, 0, me.z - b.centre.z}) > 200.0f) {  // (a new level / a jump: place it again)
                    b.centre = me;
                    b.pos = me + Vec3{std::cos(b.circleAng) * b.circleR, b.circleH, std::sin(b.circleAng) * b.circleR};
                    b.vel = Vec3{};
                }
                const Vec3 to{me.x - b.centre.x, 0, me.z - b.centre.z};
                const float d = length(to);
                if (d > 1.0f) b.centre = b.centre + to * (std::min(d, 3.0f * dt) / d);
                b.circleAng += b.circleSpeed * dt;
                const float g = std::max(ground(b.centre.x, b.centre.z), player_.seaLevel);
                const Vec3 want = Vec3{b.centre.x + std::cos(b.circleAng) * b.circleR, std::max(g, me.y) + b.circleH + 3.0f * std::sin(b.t * 0.3f),
                                       b.centre.z + std::sin(b.circleAng) * b.circleR};
                const Vec3 v = (want - b.pos) * (1.0f / std::max(dt, 1e-3f));
                b.vel = b.vel + (v - b.vel) * std::min(1.0f, dt * 2.0f);
                b.pos = b.pos + b.vel * dt;
                const Vec3 h{b.vel.x, 0, b.vel.z};
                if (length(h) > 0.5f) b.yaw = std::atan2(-h.x, -h.z);
                b.bank = -std::copysign(0.35f, b.circleSpeed);
                static const bool trace = std::getenv("GVR_BIRD_TRACE") != nullptr;
                if (trace && int((b.t - dt) / 2.0f) != int(b.t / 2.0f)) {
                    const Vec3 r = b.pos - me;
                    LOGI("Birds: Lobird %d at %.0f m, %.0f deg up, bearing %.0f deg", b.slot, length(r),
                         std::atan2(r.y, length(Vec3{r.x, 0, r.z})) * 57.3f, std::atan2(-r.x, -r.z) * 57.3f);
                }
                continue;
            }
        }
        if (b.state == kPecking || b.state == kHopping) {
            bool spook = length(Vec3{b.pos.x - me.x, 0, b.pos.z - me.z}) < 7.0f * big && std::fabs(b.pos.y - me.y) < 6.0f * big;
            spook |= fired && length(b.pos - me) < 35.0f;
            for (const Vec3& s : scares) spook |= length(s - b.pos) < 40.0f;
            for (const auto& e : enemies_.enemies)
                if (e.state != EnemyState::Dead && length(e.pos - b.pos) < 8.0f) spook = true;
            if (spook) {
                // The whole flock goes, raggedly, away from the danger.
                const char* caw = frand() < 0.5f ? "cr_caw1" : "cr_caw2";
                sfx(caw, b.pos, 0.7f, 0.9f + 0.25f * frand());
                for (Bird& o : birds_)
                    if (!o.gone && o.flock == b.flock && (o.state == kPecking || o.state == kHopping)) {
                        o.state = kFlying;
                        o.stateT = -0.3f * frand();
                        o.centre = me;
                    }
                LOGI("Birds: flock %d takes off", b.flock);
                continue;
            }
            if (b.t >= b.clipLen) {
                b.t = 0;
                if (b.state == kHopping) b.state = kPecking;
                const float r = frand();
                if (r < 0.25f) {
                    b.state = kHopping;
                    b.clip = kHop;
                    b.yaw += (frand() - 0.5f) * 2.0f;
                } else
                    b.clip = r < 0.6f ? kPeck1 : kPeck2;
                b.clipLen = std::max(0.1f, birdModels_[size_t(b.slot)]->animDuration(birdClip_[b.clip]));
            }
            if (b.state == kHopping) {
                const Vec3 np = b.pos + fwdOf(b.yaw) * (0.5f * dt / b.clipLen);
                const float y = ground(np.x, np.z);
                if (y > player_.seaLevel + 0.3f && std::fabs(y - b.pos.y) < 0.4f) b.pos = Vec3{np.x, y, np.z};
            }
            continue;
        }
        if (b.state == kFlying) {
            if (b.stateT < 0) continue;  // (a moment's lag behind the first)
            if (b.clip != kFly && b.clip != kGlide) {
                b.clip = kFly;
                b.t = 0;
                // Away from the danger, a little spread.
                Vec3 away{b.pos.x - b.centre.x, 0, b.pos.z - b.centre.z};
                if (length(away) < 0.1f) away = fwdOf(b.yaw);
                away = normalize(away);
                const float a = (frand() - 0.5f) * 0.8f;
                away = Vec3{away.x * std::cos(a) - away.z * std::sin(a), 0, away.x * std::sin(a) + away.z * std::cos(a)};
                b.vel = away * 3.0f + Vec3{0, 5.5f + frand() * 1.5f, 0};
                b.yaw = std::atan2(-away.x, -away.z);
            }
            const Vec3 h = normalize(Vec3{b.vel.x, 0, b.vel.z});
            const float hs = length(Vec3{b.vel.x, 0, b.vel.z});
            const float g = std::max(ground(b.pos.x, b.pos.z), player_.seaLevel);
            const float climbTo = g + 18.0f + 6.0f * float(b.slot % 3);
            b.vel = b.vel + h * (std::max(0.0f, 11.0f - hs) * dt * 1.2f);
            b.vel.y += ((b.pos.y < climbTo ? 4.0f : 0.0f) - b.vel.y) * std::min(1.0f, dt * 1.5f);
            b.pos = b.pos + b.vel * dt;
            b.bank = std::sin(b.t * 1.7f + float(b.slot)) * 0.25f;
            // Flap while climbing, glide once up.
            const int want = (b.pos.y < climbTo - 3.0f || std::fmod(b.stateT, 4.0f) < 1.5f) ? kFly : kGlide;
            if (want != b.clip && b.t >= b.clipLen) { b.clip = want; b.t = 0; }
            b.clipLen = std::max(0.1f, birdModels_[size_t(b.slot)]->animDuration(birdClip_[b.clip]));
            if (b.t >= b.clipLen) b.t = std::fmod(b.t, b.clipLen);
            if (length(Vec3{b.pos.x - me.x, 0, b.pos.z - me.z}) > 95.0f * big) b.gone = true;
            continue;
        }
        if (b.state == kFalling) {
            b.vel.y -= 9.8f * dt;
            b.vel = b.vel * (1.0f - 0.3f * dt);
            b.pos = b.pos + b.vel * dt;
            b.bank += dt * 5.0f;
            const float g = ground(b.pos.x, b.pos.z);
            if (b.pos.y < player_.seaLevel && g < player_.seaLevel) {
                waterSplash(Vec3{b.pos.x, player_.seaLevel, b.pos.z}, b.kind == 1 ? 0.6f : 0.25f, 1);
                b.gone = true;
            } else if (b.pos.y <= g) {
                b.pos.y = g;
                b.state = kDead;
                b.stateT = 0;
                b.bank = (frand() - 0.5f) * 0.6f;  // (face down, wings spread on the ground)
            }
            continue;
        }
        if (b.state == kDead && (b.stateT > 45.0f || length(b.pos - me) > 140.0f)) b.gone = true;
    }
    // Feathers: a puff, then a slow swaying drift down.
    for (Feather& f : feathers_) {
        f.age += dt;
        f.vel = f.vel * (1.0f - std::min(1.0f, 3.0f * dt));
        f.vel.y = std::max(f.vel.y - 2.0f * dt, -0.7f);
        f.sway += dt * 2.5f;
        f.pos = f.pos + (f.vel + Vec3{std::cos(f.sway) * 0.4f, 0, std::sin(f.sway * 0.7f) * 0.4f}) * dt;
        f.yaw += f.spin * dt;
        const float g = ground(f.pos.x, f.pos.z);
        if (f.pos.y < g + 0.02f) { f.pos.y = g + 0.02f; f.vel = Vec3{}; f.spin = 0; }
    }
    feathers_.erase(std::remove_if(feathers_.begin(), feathers_.end(), [](const Feather& f) { return f.age > f.life; }), feathers_.end());
    birds_.erase(std::remove_if(birds_.begin(), birds_.end(), [](const Bird& b) { return b.gone && b.kind == 0; }), birds_.end());

    // Pose the visible ones.
    for (Bird& b : birds_) {
        if (b.gone || length(b.pos - me) > 130.0f * big) continue;
        const int clip = b.kind == 1 ? 0 : birdClip_[b.clip];
        float t = b.t;
        if (b.state == kFalling || b.state == kDead) t = 0.1f;
        birdModels_[size_t(b.slot)]->poseBlend(renderer_, clip, t, clip, t, 0.0f);
    }
}

void GameApp::renderBirds(const Vec3& eye) {
    if (!birdsReady_) return;
    for (const Bird& b : birds_) {
        if (b.gone || length(b.pos - eye) > 160.0f) continue;
        float pitch = 0;
        if (b.state == kFlying) pitch = std::max(-0.5f, std::min(0.5f, std::atan2(b.vel.y, std::max(0.5f, length(Vec3{b.vel.x, 0, b.vel.z}))))) * 0.6f;
        const Mat4 xf = Mat4::translation(b.pos + Vec3{0, b.state == kDead ? 0.05f : birdLift_[b.kind], 0}) *
                        Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, b.yaw)) * Mat4::rotation(Quat::fromAxisAngle({1, 0, 0}, pitch)) *
                        Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, b.bank));
        birdModels_[size_t(b.slot)]->render(renderer_, xf);
    }
    if (featherModel_)
        for (const Feather& f : feathers_) {
            const float a = std::min(1.0f, (f.life - f.age) * 1.5f);
            drawModel(featherModel_, Mat4::translation(f.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, f.yaw)) *
                                         Mat4::rotation(Quat::fromAxisAngle({1, 0, 0}, std::sin(f.sway) * 0.6f)) * Mat4::scale(Vec3{0.45f, 0.45f, 0.45f}),
                      false, a);
        }
}

}  // namespace gvr
