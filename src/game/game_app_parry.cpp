// GameApp, rework: deflecting Reaper bolts with Delphi's sword (first person). A bolt that meets the
// blade is knocked away with a spark and a ping; met with a swing (blade moving), it goes back the
// way it came at the Reaper nearest its line - "return to sender" - and hurts him (it becomes one of
// your shots). GVR_PARRY_TRACE logs each one.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"

namespace gvr {

namespace {
// Closest distance between segments p0-p1 and q0-q1.
float segSegDistance(const Vec3& p0, const Vec3& p1, const Vec3& q0, const Vec3& q1) {
    const Vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s = 0, t = 0;
    if (a < 1e-8f && e < 1e-8f) return length(r);
    if (a < 1e-8f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = dot(d1, r);
        if (e < 1e-8f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = dot(d1, d2), den = a * e - b * b;
            s = den > 1e-8f ? std::clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = std::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1) { t = 1; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }
    return length((p0 + d1 * s) - (q0 + d2 * t));
}
}  // namespace

int GameApp::parryShots(const Vec3& hilt, const Vec3& tip, const Vec3& tipVel, int hand, float dt) {
    if (dt <= 0 || !playMode_ || dying_) return 0;
    static const bool trace = std::getenv("GVR_PARRY_TRACE") != nullptr;
    auto& ps = enemyFire_.projectiles();
    const float reach = 0.12f * heroScale_;
    const float swing = length(tipVel);
    int n = 0;
    for (size_t i = 0; i < ps.size();) {
        const Projectile& p = ps[i];
        // The bolt's path around now (it may have moved before or after the blade this frame).
        const Vec3 s0 = p.pos - p.vel * dt, s1 = p.pos + p.vel * (1.5f * dt);
        if (segSegDistance(hilt, tip, s0, s1) > reach) { ++i; continue; }
        const Vec3 in = normalize(p.vel);
        Vec3 out;
        int aimed = -1;
        if (swing > 1.5f) {
            // A swing: back to the Reaper nearest the line it came along (within 60 m).
            float best = 1e9f;
            for (size_t k = 0; k < enemies_.enemies.size(); ++k) {
                const Enemy& e = enemies_.enemies[k];
                if (e.state == EnemyState::Dead) continue;
                const Vec3 c = e.pos + Vec3{0, 1.1f, 0} - p.pos;
                const float along = -dot(c, in);
                if (along < 1.0f || along > 60.0f) continue;
                const float off = length(c + in * along);
                if (off < 0.25f * along + 2.0f && off < best) { best = off; aimed = int(k); }
            }
        }
        if (aimed >= 0) {
            // Led: where he will be when it gets there.
            const Enemy& e = enemies_.enemies[size_t(aimed)];
            Vec3 at = e.pos + Vec3{0, 1.1f, 0};
            at = at + Vec3{e.vel.x, 0, e.vel.z} * (length(at - p.pos) / 45.0f);
            out = normalize(at - p.pos);
            if (trace) LOGI("parry: back at Reaper %u, %.1f m", e.uid, length(at - p.pos));
        } else {
            // Knocked away: mirrored about the blade, thrown up a little, never back into you.
            Vec3 ax = normalize(tip - hilt);
            Vec3 nrm = in - ax * dot(in, ax);
            if (length(nrm) < 1e-3f) nrm = Vec3{0, 1, 0};
            nrm = normalize(nrm);
            out = normalize(in - nrm * (2.0f * dot(in, nrm)) + Vec3{0, 0.3f, 0} + (swing > 0.5f ? normalize(tipVel) * 0.5f : Vec3{0, 0, 0}));
            if (dot(out, in) > 0.2f) out = normalize(out - in * 1.5f);
        }
        Projectile q = p;
        q.vel = out * 45.0f;
        q.pos = p.pos + out * 0.2f;
        q.travelled = 0;
        q.life = 2.5f;
        q.damage = aimed >= 0 ? 2.0f : 1.0f;
        weapon_.adopt(q);
        addFlash(p.pos, Vec3{1.0f, 0.75f, 0.35f}, 3.0f, 0.12f);
        addSparks(p.pos, out, 10);
        sfx("sr_swht1", p.pos, 1.0f, 1.5f + 0.2f * frand());
        haptics_.push_back({hand, 0.9f, 0.06f});
        ++parries_;
        if (trace) LOGI("parry: a bolt off the blade (swing %.1f m/s) %s", swing, aimed >= 0 ? "back at a Reaper" : "knocked away");
        ps.erase(ps.begin() + long(i));
        ++n;
    }
    return n;
}

}  // namespace gvr
