// Player weapon: fire timing, projectiles and their hits, short-lived visual effects.
// Renderer-independent (the game app draws projectiles/effects with the original models).
#pragma once

#include <functional>
#include <vector>

#include "core/math.h"

namespace gvr {

struct Projectile {
    Vec3 pos, vel;
    float life = 0;
    float travelled = 0;
};

struct WeaponEffect {
    enum Kind { Muzzle, Impact } kind;
    Vec3 pos, dir;   // dir: muzzle forward / impact surface normal
    float age = 0, duration = 0;
    float progress() const { return duration > 0 ? age / duration : 1.0f; }
};

class WeaponSystem {
public:
    float projectileSpeed = 70.0f;  // m/s
    float fireInterval = 0.2f;      // s between shots while the trigger is held
    float range = 250.0f;           // m
    // World hit test along a segment: origin, unit dir, max distance -> hit distance + normal.
    std::function<bool(const Vec3&, const Vec3&, float, float&, Vec3&)> raycast;

    // Advances projectiles/effects; fires if the trigger is held and the weapon is ready.
    // Returns true if a shot was fired this frame.
    bool update(float dt, bool triggerHeld, const Vec3& muzzle, const Vec3& forward);

    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<WeaponEffect>& effects() const { return effects_; }
    size_t shotsFired() const { return shots_; }
    size_t hits() const { return hits_; }
    float lastHitDistance() const { return lastHitDist_; }

private:
    std::vector<Projectile> projectiles_;
    std::vector<WeaponEffect> effects_;
    float cooldown_ = 0;
    size_t shots_ = 0, hits_ = 0;
    float lastHitDist_ = 0;
};

}  // namespace gvr
