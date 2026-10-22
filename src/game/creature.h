// Ripper AI (renderer-independent, unit-tested).
//
// Creatures come from the mission setup stream at their original positions, sizes and headings
// and use the original animation clips (by the type's animation slots). Data-driven from the
// type records (xx_ripper.bin): hit points (type op 0x31: small 20, medium 40, large 90), size.
// From the user's description of the original: melee bites, and the medium/large Rippers spit
// exploding balls (efx_ripper_mspit orange / efx_ripper_lspit blue); killed Rippers either
// collapse or burst into gore. Movement, ranges, timings and damage numbers are ours (the
// original AI is native code that is not decoded: BLOCKERS.md B9).
//   Idle -> Chase (steers around obstacles, turns before running, spreads around the player)
//        -> Attack (bite) / Lunge (jump attack) / Spit (ranged) ; Hit ; Return (leash) ; Dead.
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

#include "core/math.h"
#include "scene/collision.h"

namespace gvr {

enum class CreatureClip : int { Idle = 0, Walk, Attack, Lunge, Hit, HitSmall, Die, Spit, Count };
enum class CreatureState : int { Idle, Chase, Attack, Lunge, Spit, Hit, Return, Dead };

// Ripper size class (sound set rs_/rm_/rl_, spit model mspit/lspit).
enum class CreatureSize : int { Small = 0, Medium = 1, Large = 2 };

struct Creature {
    Vec3 pos, home;
    float yaw = 0;          // 0 = facing -Z (original models' forward)
    float scale = 1;        // mission scale: size, reach and speed grow with it
    float health = 40, maxHealth = 40;
    CreatureSize size = CreatureSize::Medium;
    bool canSpit = true;
    CreatureState state = CreatureState::Idle;
    float stateTime = 0;
    CreatureClip clip = CreatureClip::Idle, prevClip = CreatureClip::Idle;
    float clipTime = 0, prevTime = 0, fade = 1e9f, rate = 1;
    bool loopClip = true;
    bool struck = false;    // this attack already dealt its damage / launched its spit
    float lungeWait = 0;    // until the next lunge is allowed
    float biteWait = 0;     // until the next bite is allowed
    float spitWait = 0;     // until the next spit is allowed
    float speed = 0;        // current ground speed (m/s), eases toward the target speed
    float alertIn = -1;     // >= 0: a pack-mate noticed the player; joins in after this delay
    float surround = 0;     // angle (rad) around the player this creature closes in from
    float detour = 0, detourTime = 0, blockedTime = 0;
    bool exploded = false;  // died by bursting (the body is gone; the game shows gore)
    float sink = 0;
    bool remove = false;
    int slot = -1;          // renderer model (owned by the game app)
    bool hunter = false;    // spawned by a nest: goes straight for the player, never gives up
    float fadeWeight(float crossfade) const { return crossfade > 0 ? std::min(1.0f, fade / crossfade) : 1.0f; }
};

struct CreatureParams {
    float sightRange = 25.0f;     // m: notices the player
    float packRange = 15.0f;      // m: Rippers this close to one that notices the player join in
    float leash = 45.0f;          // m from home: gives up and walks back
    float runSpeed = 3.0f;        // m/s at scale 1 (speed grows with sqrt(scale))
    float accel = 8.0f;           // m/s^2 toward the target speed
    float walkSpeed = 1.0f;       // m/s returning home
    float runPlayback = 2.2f;     // walk-clip playback rate at full run speed
    float reach = 1.5f;           // m at scale 1: bite range (centre to player axis)
    float biteDamage = 5.0f;
    float biteAt = 0.5f;          // fraction of the attack clip when the bite lands
    float bitePause = 0.8f;       // s between bites
    float lungeMin = 3.0f, lungeMax = 6.0f;  // m: lunge window (scale 1)
    float lungeSpeed = 6.5f;      // m/s during the airborne part of the lunge
    float lungeDamage = 10.0f;
    float lungeCooldown = 4.0f;   // s
    float spitMin = 7.0f, spitMax = 28.0f;   // m: spitting range
    float spitCooldown = 3.5f;    // s (plus up to 2 s random)
    float spitAt = 0.55f;         // fraction of the spit clip when the ball leaves the mouth
    float spitSpeed = 15.0f;      // m/s horizontal launch speed
    float spitGravity = 12.0f;    // m/s^2 on the ball (a visible lob)
    float spitDamage[3] = {0.0f, 8.0f, 15.0f};   // direct-hit damage by size (splash falls off)
    float spitRadius[3] = {0.0f, 1.8f, 2.6f};    // m: blast radius by size
    float burstChance = 0.4f;     // a killed Ripper bursts instead of collapsing
    float maxSlope = 1.19f;       // rise/run, as for the player
    float maxStepUp = 0.6f;
    float turnRate = 5.0f;        // rad/s
    float radius = 0.45f, height = 1.0f;  // capsule at scale 1 (m)
    float crossfade = 0.15f;
    float corpseTime = 8.0f;
};

struct CreatureEvent {
    enum Kind { Alert, Swing, Strike, Lunge, SpitLaunch, SpitBurst, Hurt, Die } kind;
    int index;      // creature index (spit events: the shooter's index at launch, may be stale)
    float damage;   // Strike / SpitBurst: damage dealt to the player
    Vec3 pos;
    CreatureSize size = CreatureSize::Medium;
    bool burst = false;  // Die: burst into gore instead of collapsing
    Vec3 dir;            // Die: direction of the killing shot (gore flies that way)
};

// A spit ball in flight.
struct CreatureSpit {
    Vec3 pos, vel;
    CreatureSize size;
    float age = 0;
};

class CreatureDirector {
public:
    CreatureParams params;
    std::function<float(float, float)> groundHeight;
    const CollisionWorld* collision = nullptr;
    // Optional: world position of creature i's mouth (spit origin) from its animated skeleton.
    std::function<bool(int, Vec3&)> mouth;
    float clipDuration[int(CreatureClip::Count)] = {1, 1, 1, 1, 1, 1, 1, 1};
    std::vector<Creature> creatures;
    std::vector<CreatureSpit> spits;

    int spawn(const Vec3& pos, float yaw, float scale, CreatureSize size, float hitPoints, bool canSpit);
    // A Ripper born from a nest: chases the player at once and never returns home.
    int spawnHunter(const Vec3& pos, float yaw, float scale, CreatureSize size, float hitPoints);
    // Advances AI and spit balls; appends what happened this frame to `events` (cleared first).
    void update(float dt, const Vec3& playerPos, bool playerAlive, std::vector<CreatureEvent>& events);
    // Applies damage; returns true if this killed the creature. `dir`: direction of the shot.
    bool damage(int index, float amount, const Vec3& dir, std::vector<CreatureEvent>* events = nullptr);
    // Closest living creature hit along origin + dir * t (t <= maxDist).
    bool raycast(const Vec3& origin, const Vec3& dir, float maxDist, float& t, Vec3& normal, int& index) const;
    size_t aliveCount() const;

private:
    void setClip(Creature& c, CreatureClip clip, bool loop, float rate = 1.0f);
    bool tryMove(Creature& c, const Vec3& step);
    bool pathClear(const Creature& c, float heading, float dist) const;
    void launchSpit(int i, const Creature& c, const Vec3& target, std::vector<CreatureEvent>& events);
    void updateSpits(float dt, const Vec3& player, bool playerAlive, std::vector<CreatureEvent>& events);
    unsigned rng_ = 777u;
    float rnd() { rng_ = rng_ * 1664525u + 1013904223u; return float(rng_ >> 8) / 16777216.0f; }
};

}  // namespace gvr
