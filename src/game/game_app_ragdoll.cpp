// Rework (user: "Try to implement ragdolls somehow"): dead Reapers fall as ragdolls (game/ragdoll.h).
// A body blown up, flung or killed in the air goes limp at once with the push it got; one shot dead
// plays the first moments of its death clip (the reaction) and then crumples with the shot's push.
// A body lying still sleeps (no more posing); a later blast wakes it. Delphi's sword pins one
// (game_app_delphi.cpp). Our own design.
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/game_app.h"
#include "scene/model_cache.h"

namespace gvr {

Mat4 GameApp::ragdollToWorld(const Enemy& e) const {
    // Model space (game units, Z-up) -> runtime world: the slot's render transform, plus the sink
    // (a sinking corpse sinks as a whole).
    const float lift = e.slot >= 0 && size_t(e.slot) < slotLift_.size() ? slotLift_[size_t(e.slot)] : 0.0f;
    Mat4 c = Mat4::identity();
    const float s = gGameUnitsToMeters;
    c.m[0] = -s; c.m[5] = 0; c.m[6] = s; c.m[9] = s; c.m[10] = 0;
    return Mat4::translation(e.pos + Vec3{0, lift + e.sink, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, e.yaw)) * c;
}

Ragdoll* GameApp::ragdollOf(const Enemy& e) {
    if (e.slot < 0 || size_t(e.slot) >= ragdolls_.size()) return nullptr;
    Ragdoll& r = ragdolls_[size_t(e.slot)];
    return (r.active || r.pending) && r.uid == e.uid ? &r : nullptr;
}

Ragdoll* GameApp::startRagdoll(Enemy& e, const Vec3& vel, const Vec3& spin) {
    if (ragdolls_.size() != enemyPool_.size()) ragdolls_.resize(enemyPool_.size());
    static const bool off = std::getenv("GVR_NO_RAGDOLL") != nullptr;
    if (off || e.state != EnemyState::Dead || e.posted || e.slot < 0 || size_t(e.slot) >= ragdolls_.size()) return nullptr;
    Ragdoll& r = ragdolls_[size_t(e.slot)];
    r.stop();
    r.pending = true;
    r.posedAsleep = false;
    r.uid = e.uid;
    r.clip = int(e.clip);
    r.startVel = vel;
    r.startSpin = spin;
    e.ragChecked = e.ragdoll = true;
    e.vel = e.kick = Vec3{0, 0, 0};
    return &r;
}

void GameApp::updateRagdolls(float dt) {
    if (ragdolls_.size() != enemyPool_.size()) ragdolls_.resize(enemyPool_.size());
    static const bool off = std::getenv("GVR_NO_RAGDOLL") != nullptr;
    static const bool trace = std::getenv("GVR_RAGDOLL_TRACE") != nullptr;
    for (auto& e : enemies_.enemies) {
        if (e.state != EnemyState::Dead) {
            e.kick = e.kick * std::max(0.0f, 1.0f - 6.0f * dt);
            e.ragdoll = false;
            continue;
        }
        if (off || e.posted || e.slot < 0 || size_t(e.slot) >= ragdolls_.size() || e.gibbed) continue;
        Ragdoll* rd = ragdollOf(e);
        if (!rd) {
            e.ragdoll = false;
            if (e.ragChecked || e.uid == impaledUid_) continue;
            const float k = length(e.kick);
            const bool now = k > 4.0f || e.clip == EnemyClip::Blown || e.clip == EnemyClip::FallLand;
            if (!now && e.stateTime < 0.35f) continue;  // the reaction first
            Vec3 v = e.kick * 0.7f + Vec3{e.vel.x, 0, e.vel.z} * 0.5f;
            Vec3 h{v.x, 0, v.z};
            float hl = length(h);
            if (hl > 7.0f) { h = h * (7.0f / hl); hl = 7.0f; }  // (thrown a few metres, not across the map)
            v = Vec3{h.x, std::min(6.0f, v.y), h.z};
            if (e.clip == EnemyClip::Blown) v.y = std::max(v.y, std::min(6.0f, 2.5f + 0.25f * k));
            startRagdoll(e, v, hl > 0.1f ? cross(Vec3{0, 1, 0}, h * (1.0f / hl)) * std::min(4.0f, 0.8f + 0.35f * hl) : Vec3{0, 0, 0});
            if (trace) LOGI("ragdoll: Reaper %u goes limp (push %.1f m/s%s)", e.uid, k, now ? ", at once" : "");
            continue;
        }
        e.ragdoll = true;
        e.vel = Vec3{0, 0, 0};
        if (rd->active && length(e.kick) > 0.5f) {
            rd->kick((e.kick + Vec3{0, 0.4f * length(e.kick), 0}) * 0.6f);
            rd->posedAsleep = false;
        }
        e.kick = Vec3{0, 0, 0};
        if (rd->active && !rd->asleep) {
            rd->step(dt, enemies_.groundHeight);
            const Vec3 c = rd->centre();
            e.pos.x = c.x;
            e.pos.z = c.z;
            if (rd->asleep && trace) LOGI("ragdoll: Reaper %u lies still (%.1f s) at %.1f %.1f %.1f, %.1f m from the player", e.uid, rd->age, c.x, rd->lowest(), c.z, length(c - player_.position));
        }
    }
}

}  // namespace gvr
