#include "game/weapon.h"

#include <algorithm>

namespace gvr {

bool WeaponSystem::update(float dt, bool triggerHeld, const Vec3& muzzle, const Vec3& forward) {
    // Effects age and expire.
    for (auto& e : effects_) e.age += dt;
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(), [](const WeaponEffect& e) { return e.age >= e.duration; }),
                   effects_.end());

    // Projectiles: swept segment test against the world each frame (no tunnelling).
    for (auto& p : projectiles_) {
        const Vec3 step = p.vel * dt;
        const float len = length(step);
        p.life -= dt;
        if (len <= 0) continue;
        const Vec3 dir = step * (1.0f / len);
        float t;
        Vec3 n;
        int target = -1;
        if (raycast && raycast(p.pos, dir, len, t, n, target)) {
            const Vec3 hp = p.pos + dir * t;
            if (onSegment) onSegment(p.pos, hp, dir);
            effects_.push_back({WeaponEffect::Impact, hp + n * 0.05f, n, 0.0f, 0.25f, p.style});
            p.life = -1.0f;
            ++hits_;
            lastHitDist_ = p.travelled + t;
            hitDamage_ = p.damage;
            hitStyle_ = p.style;
            hitTag_ = p.tag;
            if (onHit) onHit(target, hp, dir);
            hitDamage_ = 1.0f;
            hitStyle_ = 0;
            hitTag_ = -1;
        } else {
            if (onSegment) onSegment(p.pos, p.pos + step, dir);
            p.pos += step;
            p.travelled += len;
        }
    }
    projectiles_.erase(std::remove_if(projectiles_.begin(), projectiles_.end(), [](const Projectile& p) { return p.life <= 0; }),
                       projectiles_.end());

    // Firing.
    cooldown_ = std::max(0.0f, cooldown_ - dt);
    if (!triggerHeld || cooldown_ > 0) return false;
    cooldown_ = fireInterval;
    spawnShot(muzzle, forward);
    return true;
}

void WeaponSystem::spawnShot(const Vec3& muzzle, const Vec3& forward) {
    const Vec3 f = normalize(forward);
    projectiles_.push_back({muzzle, f * projectileSpeed, range / projectileSpeed, 0.0f, shotDamage, shotStyle, shotTag});
    if (shotStyle != 3) effects_.push_back({WeaponEffect::Muzzle, muzzle, f, 0.0f, shotStyle == 2 ? 0.12f : 0.07f, shotStyle});  // no flash off a bow
    ++shots_;
}

}  // namespace gvr
