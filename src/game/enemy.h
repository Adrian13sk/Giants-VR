// Enemy AI for Reaper grunts (renderer-independent, unit-tested).
//
// State machine per enemy: Idle (scanning) -> Advance (walk toward the player while in sight)
// -> Fire (stand and shoot bursts within range) ; Hit (short reaction) ; Dead (death clip,
// body sinks away). The director also answers ray queries against the enemies' capsules.
// Behaviour and numbers are ours (the original AI scripts are not decoded): see BLOCKERS.md.
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "core/math.h"
#include "scene/collision.h"

namespace gvr {

enum class EnemyClip : int { Idle = 0, Walk, Fire, Hit1, Hit2, Die1, Die2, Die3, Count };
enum class EnemyState : int { Idle, Advance, Fire, Hit, Dead };

struct Enemy {
    Vec3 pos;
    float yaw = 0;          // 0 = facing -Z (original models' forward)
    float health = 100;
    EnemyState state = EnemyState::Idle;
    float stateTime = 0;
    EnemyClip clip = EnemyClip::Idle, prevClip = EnemyClip::Idle;
    float clipTime = 0, prevTime = 0, fade = 1e9f;
    bool loopClip = true;
    float fireCooldown = 0;
    int burstLeft = 0;
    float sink = 0;         // metres sunk after death
    bool remove = false;
    int slot = -1;          // renderer model slot (owned by the game app)
    float fadeWeight(float crossfade) const { return crossfade > 0 ? std::min(1.0f, fade / crossfade) : 1.0f; }
};

struct EnemyParams {
    float sightRange = 45.0f;     // m: notices the player
    float fireRange = 22.0f;      // m: stops to shoot
    float walkSpeed = 1.65f;      // m/s (rpman_walk_fire stride: 8.2 units/s)
    float turnRate = 5.0f;        // rad/s
    float fireInterval = 0.35f;   // s between shots in a burst
    int burstSize = 3;
    float burstPause = 1.4f;      // s
    float radius = 0.45f, height = 2.2f;  // capsule (m)
    float crossfade = 0.2f;
    float corpseTime = 10.0f;     // s before the body sinks away
};

class EnemyDirector {
public:
    EnemyParams params;
    std::function<float(float, float)> groundHeight;
    const CollisionWorld* collision = nullptr;
    float clipDuration[int(EnemyClip::Count)] = {1, 1, 1, 1, 1, 1, 1, 1};
    std::vector<Enemy> enemies;

    int spawn(const Vec3& pos, float yaw);
    // Advances AI. `shooters` receives the indices of enemies that fire a shot this frame.
    void update(float dt, const Vec3& playerPos, bool playerAlive, std::vector<int>& shooters);
    // Applies damage; returns true if this killed the enemy.
    bool damage(int index, float amount);
    // Closest capsule hit along origin + dir * t (t <= maxDist); living and dead bodies.
    bool raycast(const Vec3& origin, const Vec3& dir, float maxDist, float& t, Vec3& normal, int& index) const;
    size_t aliveCount() const;

private:
    void setClip(Enemy& e, EnemyClip c, bool loop);
    unsigned rng_ = 12345u;
    unsigned nextRand() { rng_ = rng_ * 1664525u + 1013904223u; return rng_ >> 8; }
};

// Segment (origin, unit dir, maxDist) vs vertical capsule (axis a..b, radius r): entry distance.
bool rayCapsule(const Vec3& o, const Vec3& d, float maxDist, const Vec3& a, const Vec3& b, float r, float& t);

}  // namespace gvr
