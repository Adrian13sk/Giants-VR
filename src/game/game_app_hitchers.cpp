// GameApp, rework: hitchers. xx_birds.gzp also holds "hitcher" - a big blue, red-beaked perching
// bird with an idle and a fly clip - unused by the levels. Its name says what it does: it rides.
// Ours: a few sit on the backs of grazing Vimps, riding along as they walk; when its Vimp runs,
// is hit or dies, the hitcher flaps off, circles a while and later lands on a calm Vimp again.
// Shot or stoned it falls like the other birds (feathers, a squawk). Our own behaviour on the art.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/game_app.h"
#include "scene/asset_world.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr int kMaxHitchers = 6;
enum { kRide, kAway, kLanding, kFall, kLie };
}  // namespace

void GameApp::initHitchers(AssetWorld* w, ModelCache* mc) {
    hitchers_.clear();
    if (!w || !mc || vimps_.vimps.empty() || vimpModels_.empty() || std::getenv("GVR_NO_HITCHERS")) return;
    if (hitcherModels_.size() != size_t(kMaxHitchers) || hitcherCache_ != mc) {
        hitcherModels_.clear();
        hitcherCache_ = mc;
        for (int i = 0; i < kMaxHitchers; ++i) {
            auto m = std::make_unique<SkinnedModel>();
            if (!m->load(w->fs(), *mc, renderer_, "hitcher", "hitcher_idle.anm")) { hitcherModels_.clear(); return; }
            hitcherFly_ = std::max(0, m->addAnimation(w->fs(), "hitcher_fly.anm"));
            if (i == 0) {
                Vec3 mn, mx;
                m->boundsAt(0, 0.0f, mn, mx);
                hitcherLift_ = -mn.y;
                LOGI("Hitchers: model %.2f x %.2f x %.2f m", mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
            }
            m->pose(renderer_, 0.0f);
            hitcherModels_.push_back(std::move(m));
        }
    }
    // The Vimp's back: the top of its rest pose, a little lower (its hump), from the model origin.
    const SkinnedModel& vm = *vimpModels_[0];
    vimpBack_ = vm.restMax().y * 0.78f;
    // One on every other Vimp, up to the pool's size.
    for (size_t i = 0; i < vimps_.vimps.size() && int(hitchers_.size()) < kMaxHitchers; i += 2) {
        Hitcher h;
        h.vimp = int(i);
        h.slot = int(hitchers_.size());
        h.t = frand() * 2.0f;
        h.yawOff = (frand() - 0.5f) * 0.8f;
        hitchers_.push_back(h);
    }
    LOGI("Hitchers: %zu riding on Vimps (back at %.2f m)", hitchers_.size(), vimpBack_);
}

Vec3 GameApp::vimpBackPos(int i) const {
    const Vimp& v = vimps_.vimps[size_t(i)];
    return v.pos + Vec3{0, (vimpLift_ + vimpBack_) * v.growth, 0};
}

void GameApp::updateHitchers(float dt) {
    if (hitchers_.empty() || hitcherModels_.empty()) return;
    const Vec3 me = player_.position;
    auto calm = [&](int i) {
        const Vimp& v = vimps_.vimps[size_t(i)];
        return !v.eaten && v.growth > 0.8f && (v.state == VimpState::Graze || v.state == VimpState::Walk);
    };
    for (Hitcher& h : hitchers_) {
        h.t += dt;
        h.stateT += dt;
        if (h.state == kRide) {
            if (h.vimp < 0 || size_t(h.vimp) >= vimps_.vimps.size() || !calm(h.vimp) || length(vimpBackPos(h.vimp) - me) < 3.5f) {
                // Off it goes: up and away from the trouble.
                if (h.vimp >= 0 && size_t(h.vimp) < vimps_.vimps.size())
                    LOGI("Hitchers: one takes off (its Vimp %s)", vimps_.vimps[size_t(h.vimp)].state == VimpState::Flee ? "runs" :
                         (vimps_.vimps[size_t(h.vimp)].state == VimpState::Dead || vimps_.vimps[size_t(h.vimp)].state == VimpState::Hit ? "is hit" : "- you came close"));
                h.state = kAway;
                h.stateT = 0;
                const Vec3 from = h.vimp >= 0 && size_t(h.vimp) < vimps_.vimps.size() ? vimpBackPos(h.vimp) : h.pos;
                Vec3 away{from.x - me.x, 0, from.z - me.z};
                away = length(away) > 0.1f ? normalize(away) : Vec3{1, 0, 0};
                h.vel = away * 4.0f + Vec3{0, 5.0f, 0};
                h.circle = from + away * 25.0f + Vec3{0, 14.0f, 0};
                sfx(frand() < 0.5f ? "cr_caw1" : "cr_caw2", from, 0.8f, 0.75f);
                continue;
            }
            const Vimp& v = vimps_.vimps[size_t(h.vimp)];
            h.pos = vimpBackPos(h.vimp);
            h.yaw = v.yaw + h.yawOff;
            continue;
        }
        if (h.state == kAway) {
            // Circles above the place for a while, then looks for a calm Vimp.
            const float a = h.stateT * 0.6f + float(h.slot);
            const Vec3 want = h.circle + Vec3{std::cos(a) * 9.0f, 2.0f * std::sin(h.stateT * 0.4f), std::sin(a) * 9.0f};
            h.vel = h.vel + ((want - h.pos) * 1.2f - h.vel) * std::min(1.0f, dt * 1.5f);
            h.pos = h.pos + h.vel * dt;
            if (h.stateT > 18.0f) {
                int best = -1;
                float bd = 1e9f;
                for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
                    if (!calm(int(i)) || length(vimps_.vimps[i].pos - me) < 12.0f) continue;
                    bool taken = false;
                    for (const Hitcher& o : hitchers_) taken |= &o != &h && o.vimp == int(i) && (o.state == kRide || o.state == kLanding);
                    if (taken) continue;
                    const float d = length(vimps_.vimps[i].pos - h.pos);
                    if (d < bd) { bd = d; best = int(i); }
                }
                if (best >= 0 && bd < 120.0f) { h.vimp = best; h.state = kLanding; h.stateT = 0; }
                else h.stateT = 10.0f;  // (none calm: another round)
            }
        } else if (h.state == kLanding) {
            if (!calm(h.vimp)) { h.state = kAway; h.stateT = 10.0f; continue; }
            const Vec3 to = vimpBackPos(h.vimp) - h.pos;
            const float d = length(to);
            if (d < 0.3f) { h.state = kRide; h.stateT = 0; LOGI("Hitchers: one lands on a Vimp again"); continue; }
            const float sp = std::min(9.0f, 1.5f + d * 0.8f);
            h.vel = h.vel + (to * (sp / std::max(d, 0.01f)) - h.vel) * std::min(1.0f, dt * 3.0f);
            h.pos = h.pos + h.vel * dt;
        } else if (h.state == kFall) {
            h.vel.y -= 9.8f * dt;
            h.pos = h.pos + h.vel * dt;
            float g = 0;
            if (assetWorld_ && assetWorld_->terrain().heightAt(h.pos.x, h.pos.z, g) && h.pos.y <= g) { h.pos.y = g; h.state = kLie; h.stateT = 0; }
            continue;
        } else if (h.state == kLie) {
            if (h.stateT > 40.0f) { h.state = kAway; h.stateT = 0; h.pos = h.pos + Vec3{0, 60.0f, 0}; h.circle = h.pos; }  // (another one, later)
            continue;
        }
        const Vec3 hv{h.vel.x, 0, h.vel.z};
        if (length(hv) > 0.3f) h.yaw = std::atan2(-hv.x, -hv.z);
    }
    // Pose: each its own clip time (perched: the idle; in the air: flapping).
    for (Hitcher& h : hitchers_) {
        if (h.state == kLie || length(h.pos - me) > 120.0f) continue;
        const bool fly = h.state == kAway || h.state == kLanding;
        const int clip = fly ? hitcherFly_ : 0;
        const float len = std::max(0.1f, hitcherModels_[size_t(h.slot)]->animDuration(clip));
        hitcherModels_[size_t(h.slot)]->poseBlend(renderer_, clip, std::fmod(h.t * (fly ? 1.4f : 1.0f), len), clip, 0.0f, 0.0f);
    }
}

void GameApp::hitHitcher(Hitcher& h, const Vec3& dir) {
    if (h.state == kFall || h.state == kLie) return;
    h.state = kFall;
    h.stateT = 0;
    h.vimp = -1;
    h.vel = Vec3{dir.x * 3.0f, 2.0f, dir.z * 3.0f};
    sfx(frand() < 0.5f ? "cr_deat1" : "cr_deat2", h.pos, 0.9f, 0.7f);
    for (int k = 0; k < 10 && feathers_.size() < 96; ++k) {
        Feather f;
        f.pos = h.pos + Vec3{0, 0.8f, 0};
        f.vel = Vec3{(frand() - 0.5f) * 3.0f, frand() * 2.0f, (frand() - 0.5f) * 3.0f} + dir * 1.5f;
        f.yaw = frand() * 6.2832f;
        f.spin = (frand() - 0.5f) * 6.0f;
        f.sway = frand() * 6.2832f;
        f.life = 5.0f + frand() * 4.0f;
        feathers_.push_back(f);
    }
    LOGI("Hitchers: one is shot down");
}

void GameApp::renderHitchers(const Vec3& eye) {
    if (hitchers_.empty() || hitcherModels_.empty()) return;
    for (const Hitcher& h : hitchers_) {
        if (length(h.pos - eye) > 120.0f) continue;
        const bool lying = h.state == kLie || h.state == kFall;
        Mat4 xf = Mat4::translation(h.pos + Vec3{0, lying ? 0.1f : hitcherLift_ * 0.9f, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, h.yaw));
        if (lying) xf = xf * Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, 1.4f));
        hitcherModels_[size_t(h.slot)]->render(renderer_, xf);
    }
}

}  // namespace gvr
