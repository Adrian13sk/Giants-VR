// Melee wildlife AI (Rippers), renderer-independent and unit-tested.
//
// Creatures come from the mission setup stream at their original positions, sizes and
// headings, and use the original animation clips (by the type's animation slots). The
// behaviour is ours: the original AI is native code that is not decoded (BLOCKERS.md B9).
//   Idle (scans) -> Chase (runs at the player) -> Attack (bite within reach) / Lunge (jump attack
//   from a few metres) ; Hit (flinch) ; Return (walks home when the player escapes) ; Dead.
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "core/math.h"
#include "scene/collision.h"

namespace gvr {

enum class CreatureClip : int { Idle = 0, Walk, Attack, Lunge, Hit, HitSmall, Die, Count };
enum class CreatureState : int { Idle, Chase, Attack, Lunge, Hit, Return, Dead };

struct Creature {
    Vec3 pos, home;
    float yaw = 0;          // 0 = facing -Z (original models' forward)
    float scale = 1;        // mission scale: size, reach, speed and health grow with it
    float health = 50;
    CreatureState state = CreatureState::Idle;
    float stateTime = 0;
    CreatureClip clip = CreatureClip::Idle, prevClip = CreatureClip::Idle;
    float clipTime = 0, prevTime = 0, fade = 1e9f, rate = 1;
    bool loopClip = true;
    bool struck = false;    // this attack already dealt its damage
    float cooldown = 0;     // until the next lunge is allowed
    float biteWait = 0;     // until the next bite is allowed
    float sink = 0;
    bool remove = false;
    int slot = -1;          // renderer model (owned by the game app)
    int kind = 0;           // caller-defined (e.g. sound set)
    float fadeWeight(float crossfade) const { return crossfade > 0 ? std::min(1.0f, fade / crossfade) : 1.0f; }
};

struct CreatureParams {
    float sightRange = 20.0f;     // m: notices the player
    float leash = 45.0f;          // m from home: gives up and walks back
    float runSpeed = 2.8f;        // m/s at scale 1 (speed grows with sqrt(scale))
    float walkSpeed = 1.0f;       // m/s returning home
    float runPlayback = 2.2f;     // walk-clip playback rate while running
    float reach = 1.5f;           // m at scale 1: bite range (centre to player axis)
    float biteDamage = 5.0f;
    float bitePause = 0.8f;       // s between bites (stands its ground, facing the player)
    float biteAt = 0.5f;          // fraction of the attack clip when the bite lands
    float lungeMin = 3.0f, lungeMax = 6.5f;  // m: lunge window (scale 1)
    float lungeSpeed = 6.5f;      // m/s during the airborne part of the lunge
    float lungeDamage = 12.0f;
    float lungeCooldown = 4.0f;   // s
    float health = 50.0f;         // at scale 1 (scales with scale^2, capped at x4)
    float maxSlope = 1.19f;       // rise/run, as for the player
    float maxStepUp = 0.6f;
    float turnRate = 6.0f;        // rad/s
    float radius = 0.45f, height = 1.0f;  // capsule at scale 1 (m)
    float crossfade = 0.15f;
    float corpseTime = 8.0f;
};

struct CreatureEvent {
    enum Kind { Alert, Swing, Strike, Lunge, Hurt, Die } kind;
    int index;
    float damage;   // Strike: damage dealt to the player
    Vec3 pos;
};

class CreatureDirector {
public:
    CreatureParams params;
    std::function<float(float, float)> groundHeight;
    const CollisionWorld* collision = nullptr;
    float clipDuration[int(CreatureClip::Count)] = {1, 1, 1, 1, 1, 1, 1};
    std::vector<Creature> creatures;

    int spawn(const Vec3& pos, float yaw, float scale, int kind = 0);
    // Advances AI; appends what happened this frame to `events` (cleared first).
    void update(float dt, const Vec3& playerPos, bool playerAlive, std::vector<CreatureEvent>& events);
    // Applies damage; returns true if this killed the creature.
    bool damage(int index, float amount, std::vector<CreatureEvent>* events = nullptr);
    // Closest living creature hit along origin + dir * t (t <= maxDist).
    bool raycast(const Vec3& origin, const Vec3& dir, float maxDist, float& t, Vec3& normal, int& index) const;
    size_t aliveCount() const;

private:
    void setClip(Creature& c, CreatureClip clip, bool loop, float rate = 1.0f);
    bool tryMove(Creature& c, const Vec3& step);
    unsigned rng_ = 777u;
    float rnd() { rng_ = rng_ * 1664525u + 1013904223u; return float(rng_ >> 8) / 16777216.0f; }
};

}  // namespace gvr
