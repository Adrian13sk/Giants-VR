// Enemy AI for Reaper grunts (renderer-independent, unit-tested).
//
// State machine per enemy: Idle (scanning) -> Advance (walk toward the player while in sight)
// -> Fire (stand and shoot bursts within range) ; Hit (short reaction) ; Dead (death clip,
// body sinks away). The director also answers ray queries against the enemies' capsules.
// Behaviour and numbers are ours (the original AI scripts are not decoded): see BLOCKERS.md.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/math.h"
#include "scene/collision.h"

namespace gvr {

enum class EnemyClip : int { Idle = 0, Walk, Fire, Hit1, Hit2, Die1, Die2, Die3, Run, Patrol, Alert, Blown, FallCycle, FallLand, Poke,
                             KneelFire, Punch, DieHead, DieKnees, Throw, Hover, CrouchFire, HitBig, GetUp, BzkaFire, CrouchIdle, RockInto, RockBreathe, TreeHide, TreeBreathe, TreePeek, SpellCaught, SpellFloat, Count };  // (from KneelFire on: rework)
enum class EnemyState : int { Idle, Advance, Fire, Hit, Dead, Falling };

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
    float flank = 0;        // lateral offset (m) of this enemy's approach point around the player
    float flankTimer = 0;
    int burstsLeft = 0;     // bursts remaining at the current stop
    float detour = 0;       // >0 / <0: steering around an obstacle (radians added to the heading)
    float detourTime = 0;
    float blockedTime = 0;
    float sink = 0;         // metres sunk after death
    bool remove = false;
    int slot = -1;          // renderer model slot (owned by the game app)
    int objective = -1;     // mission objective this enemy's death completes (game app), -1 none
    bool posted = false;    // sentry on a platform (guard tower): never moves, fires from there
    float postY = 0;        // its floor height
    bool roofed = false;    // posted because he fell onto a roof / ledge (not a guard post): dropped if it goes
    // Delphi's SHRINK (game layer casts it: shrinkT = seconds shrunk): his size eases to a quarter and back;
    // shrunk, he runs from her, can't fire or punch, and any hit kills him.
    float shrink = 1.0f, shrinkT = 0;
    uint32_t uid = 0;       // stable id (corpses, Cleaners)
    // Corpse (corpsesPersist): lies until eaten by Cleaners or blown apart.
    float eaten = 0, corpseHp = 50;
    bool gibbed = false, cleanersCalled = false;
    // Patrol: wanders a little around its post until it notices the player.
    Vec3 home, patrolTo;
    float patrolWait = 0;
    float alert = 0;        // > 0: has noticed the player, turning to him before it acts
    float lookTimer = 0;
    bool aware = false;
    float poke = -1;        // >= 0: walking up to / poking a suspicious bush (s)
    float lost = 0;         // s an aware Reaper has had the hidden player out of sight
    float losT = 0;         // s until its line of sight to the player is re-checked
    bool los = false;       // last line-of-sight result
    Vec3 vel;               // Falling: knocked off its post (the tower collapsed under it)
    float fallFrom = 0;     // height it fell from
    float wallHit = 0;      // Falling: speed (m/s) he last hit a wall at; the game layer reads and clears it
    int postObject = -1;    // the structure it stands on (game layer's world object index)
    // Besieging (set by the game layer: a raider at a building): while unaware of the player it
    // stands facing `siegeAt` and fires bursts at it (reported in EnemyDirector::siegeShots).
    bool siege = false;
    Vec3 siegeAt;
    // His weapon (game layer): -1 not yet chosen, 0 spear gun, 1 bazooka; its transform this frame.
    int weapon = -1;
    bool weaponOn = false;
    Mat4 weaponXf = Mat4::identity();
    float rocketCool = 0;
    float hiddenT = 0, grenadeCool = 0, flinchCool = 0, retreatT = 0;
    int role = 0;           // rework: 1 = marksman (keeps his distance, single aimed shots, a glow before each)  // (retreatT: badly hurt, falling back)  // (flinchCool: near misses, game_app_vr.cpp)
    // Rework: where the last hit landed (fraction of his height: a head shot or a leg shot picks the
    // death); a one-shot action playing (a punch, a throw) holds his clip; the punch's blow pending.
    float hitHeight = 0.5f, actT = 0, punchCool = 0;
    bool punchPending = false;
    bool dropping = false;
    bool inCover = false;   // rework: running to / firing from coverAt
    bool ducked = false;    // rework: down behind its cover between bursts (rpman_rock_into, then rpman_rock_breathe)
    bool treeCover = false; // rework: its cover is a tree trunk (stands behind it, peeks out)
    bool gunArmOff = false; // rework: the right arm was cut off (no gun in the hand)
    bool disarmed = false;  // rework: his gun was shot out of his hand (game layer): he comes in to punch
    bool fetchGun = false;  // rework: disarmed, he runs to where his gun lies (gunAt) to pick it up (game layer)
    Vec3 gunAt;
    float limpT = 0;        // rework: > 0: shot in the leg, he limps (slower) this long
    Vec3 coverAt, coverFrom; // the spot, and where the player was when it was chosen  // rework: coming down on a hover pack (an ambush from the air)
    // Rework: ragdolls (game_app_ragdoll.cpp). kick: the impulses of the last moments (m/s, decaying,
    // shots, blasts, blows - also on the dead); ragdoll: the body is a ragdoll (the game layer moves it).
    Vec3 kick;
    bool ragdoll = false, ragChecked = false;
    float hitFlash = 0;     // rework: > 0 just hit (the body flashes)  // rework: the player out of sight this long -> a grenade (game layer)
    float fadeWeight(float crossfade) const { return crossfade > 0 ? std::min(1.0f, fade / crossfade) : 1.0f; }
};

struct EnemyParams {
    float sightRange = 60.0f;     // m: sees the player in front with a clear line (was 50, any side)
    float shootRange = 30.0f;     // m: stops running to shoot when this close
    float fireRange = 10.0f;      // m: closest stand-off distance
    float coverShare = 0.6f;      // rework: chance a repositioning Reaper looks for cover
    float crouchers = 0.3f;       // rework: share of the others that fire from a crouch (a smaller target)
    float marksmen = 0.25f;       // rework: share of Reapers that are marksmen (0: none)
    float runSpeed = 4.2f;        // m/s (rpman_bzka_run stride: 18.4 units/s = 3.7; the rework runs them faster)
    float walkSpeed = 1.65f;      // m/s (rpman_walk_fire stride: pressing in while firing)
    bool pressIn = true;          // rework: walk at the player while firing (false: the classic stand-and-fire)
    int burstsPerStop = 2;        // bursts fired at each stop before running to a new spot
    float maxSlope = 1.19f;       // rise/run: steeper uphill ground blocks them (50 deg, like the player)
    float maxStepUp = 0.8f;       // m
    float slideSpeed = 6.0f;      // m/s down ground too steep to stand on
    float turnRate = 5.0f;        // rad/s
    float fireInterval = 0.35f;   // s between shots in a burst
    int burstSize = 3;
    float burstPause = 1.1f;      // s (rework; was 1.4)
    float radius = 0.45f, height = 2.2f;  // capsule (m)
    float crossfade = 0.2f;
    float corpseTime = 10.0f;     // s before the body sinks away
    bool corpsesPersist = false;  // bodies stay until eaten (Enemy::eaten) or gibbed
    // Noticing the player: within sightRange and roughly in front (or very close, or shot at),
    // a moment of turning to look before it advances. Until then a short patrol near home.
    float noticeAngle = 1.4f;     // rad either side of facing
    float hearRange = 14.0f;      // m: noticed from any direction this close
    float noticeDelay = 0.45f;    // s from noticing to acting (rework; was 0.7)
    float patrolRadius = 6.0f;    // m around home
    float patrolSpeed = 1.1f;     // m/s (walk clip)
    bool punch = true;          // rework: close up he punches
};

class EnemyDirector {
public:
    EnemyParams params;
    std::function<float(float, float)> groundHeight;
    // Rework: a spot in cover (behind a rock or a tree, to one side, as seen from the player) near
    // `near`; false if there is none. Set by the game layer.
    // tree: the spot is behind a tree trunk (he hides standing: rpman_behind_tree_*), else behind a rock/wall.
    std::function<bool(const Vec3& near, const Vec3& player, Vec3& spot, bool& tree)> findCover;
    const CollisionWorld* collision = nullptr;
    float clipDuration[int(EnemyClip::Count)] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    // The player hidden in his bush pack (2b): an unaware Reaper only finds him by walking up and
    // poking the bush (rpman_poke_bush); an engaged one loses him when he's far enough.
    bool playerHidden = false;
    bool playerMoving = false;  // a moving bush draws the eye from further
    bool pokeStarted = false, pokeFound = false;  // this frame: a Reaper went to poke the bush / found him
    std::vector<Enemy> enemies;
    std::vector<int> landed;  // this frame: enemies killed by a fall (landed hard)
    std::vector<int> punches;  // this frame (rework): enemies whose punch landed on the player
    std::vector<int> siegeShots;  // this frame: besieging enemies that fired at their target

    int spawn(const Vec3& pos, float yaw);
    // Advances AI. `shooters` receives the indices of enemies that fire a shot this frame.
    void update(float dt, const Vec3& playerPos, bool playerAlive, std::vector<int>& shooters);
    // Applies damage; returns true if this killed the enemy. A hit always makes it aware.
    bool damage(int index, float amount, bool blast = false);  // blast: blown off the feet (rpman_hit_blowup)
    // Alert everyone within `radius` of `at` (gunfire, a comrade's death) to the player.
    void alertNear(const Vec3& at, float radius);
    // Knocks a sentry off its post: it falls (rpman_hit_fall_cycle) and a long fall kills it on
    // landing (rpman_hit_fall_land); a short one it gets up from.
    void knockOff(int index, const Vec3& velocity);
    // Pushes a standing enemy (blast): slides it a little; it stays on its feet.
    void push(int index, const Vec3& impulse);
    float lethalFall = 5.0f;  // m
    // Closest capsule hit along origin + dir * t (t <= maxDist); living and dead bodies.
    bool raycast(const Vec3& origin, const Vec3& dir, float maxDist, float& t, Vec3& normal, int& index) const;
    size_t aliveCount() const;
    // His body's height now: kneeling or crouched to fire, lower (hit capsule, head shots).
    float bodyHeight(const Enemy& e) const { return bodyHeightFull(e) * e.shrink; }
    float bodyHeightFull(const Enemy& e) const {
        if (e.state == EnemyState::Hit && e.clip == EnemyClip::HitBig) return params.height * 0.45f;  // knocked down
        if (e.state == EnemyState::Hit && e.clip == EnemyClip::GetUp) return params.height * 0.75f;
        if (e.state == EnemyState::Fire && (e.clip == EnemyClip::RockInto || e.clip == EnemyClip::RockBreathe)) return params.height * 0.5f;  // ducked
        if (e.state == EnemyState::Fire && (e.clip == EnemyClip::TreeHide || e.clip == EnemyClip::TreeBreathe)) return params.height * 0.25f;  // flat behind a trunk (only a sliver shows)
        return (e.clip == EnemyClip::CrouchFire || e.clip == EnemyClip::KneelFire || e.clip == EnemyClip::BzkaFire || e.clip == EnemyClip::CrouchIdle) && e.state == EnemyState::Fire ? params.height * 0.66f : params.height;
    }
    // A one-shot action clip that holds until it ends (rework: the grenade throw).
    void playAction(int i, EnemyClip c);
    // Any clip on a body (rework: a Reaper hanging on Delphi's blade flails, then lies again).
    void setBodyClip(int i, EnemyClip c, bool loop) { if (i >= 0 && size_t(i) < enemies.size()) { enemies[size_t(i)].actT = 0; setClip(enemies[size_t(i)], c, loop); } }

    bool lineOfSight(const Vec3& from, const Vec3& to) const;  // (public 2026-10-29: the mission bot test hook)

private:
    void setClip(Enemy& e, EnemyClip c, bool loop);
    // Moves e horizontally by `step` unless the ground rises too steeply (cliff); true if moved.
    bool tryMove(Enemy& e, const Vec3& step);
    unsigned rng_ = 12345u;
    uint32_t nextUid_ = 1;
    unsigned nextRand() { rng_ = rng_ * 1664525u + 1013904223u; return rng_ >> 8; }
};

// Segment (origin, unit dir, maxDist) vs vertical capsule (axis a..b, radius r): entry distance.
bool rayCapsule(const Vec3& o, const Vec3& d, float maxDist, const Vec3& a, const Vec3& b, float r, float& t);

}  // namespace gvr
