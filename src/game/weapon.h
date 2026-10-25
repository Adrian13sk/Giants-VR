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
    float damage = 1.0f;  // x the handgun's hit (the gun that fired it)
    int style = 0;        // how it looks (the game draws it): 0 handgun bolt, 1 machine gun, 2 sniper, 3 arrow
};

struct WeaponEffect {
    enum Kind { Muzzle, Impact } kind;
    Vec3 pos, dir;   // dir: muzzle forward / impact surface normal
    float age = 0, duration = 0;
    int style = 0;   // the shot's style (Projectile::style)
    float progress() const { return duration > 0 ? age / duration : 1.0f; }
};

class WeaponSystem {
public:
    float projectileSpeed = 70.0f;  // m/s
    float fireInterval = 0.2f;      // s between shots while the trigger is held
    float range = 250.0f;           // m
    float shotDamage = 1.0f;        // damage of the shots fired from now on (x the handgun's)
    int shotStyle = 0;              // and their look (Projectile::style)
    // Hit test along a segment: origin, unit dir, max distance -> hit distance, normal and
    // target id (-1 = world geometry, >= 0 or other negatives = characters, caller-defined).
    std::function<bool(const Vec3&, const Vec3&, float, float&, Vec3&, int&)> raycast;
    // Called on every hit (target id, hit point, bolt direction).
    std::function<void(int, const Vec3&, const Vec3&)> onHit;

    // Advances projectiles/effects; fires if the trigger is held and the weapon is ready.
    // Returns true if a shot was fired this frame.
    bool update(float dt, bool triggerHeld, const Vec3& muzzle, const Vec3& forward);
    // Fires one bolt now, ignoring the trigger/cooldown (AI-controlled shooters).
    void spawnShot(const Vec3& muzzle, const Vec3& forward);

    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    std::vector<Projectile>& projectiles() { return projectiles_; }  // (deflecting: taken out of here...)
    void adopt(const Projectile& p) { projectiles_.push_back(p); }   // (...and fired on from another system)
    const std::vector<WeaponEffect>& effects() const { return effects_; }
    size_t shotsFired() const { return shots_; }
    size_t hits() const { return hits_; }
    float lastHitDistance() const { return lastHitDist_; }
    // While onHit runs: the damage factor of the shot that hit (1 otherwise).
    float hitDamage() const { return hitDamage_; }

private:
    std::vector<Projectile> projectiles_;
    std::vector<WeaponEffect> effects_;
    float cooldown_ = 0;
    size_t shots_ = 0, hits_ = 0;
    float lastHitDist_ = 0;
    float hitDamage_ = 1.0f;
};

}  // namespace gvr
