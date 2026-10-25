// Vimps: the big, not-too-bright grazing beasts of the Smartie lands (vp_l1, object type 8,
// 20 hp). The original populates them with native code (its "-novimps" switch turns them off;
// herd and grazing logic named in the decompilation but not decoded), so the herds here are ours:
// a few beasts graze and wander together, startle at the Meccaryn or gunfire, stampede away as a
// herd, and calm down again. Killed, a Vimp drops pieces of Vimp meat (the game layer's pickups -
// Story 2's first mission asks for 20). Renderer-independent; the game layer poses the vp_* clips.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/math.h"

namespace gvr {

enum class VimpClip : int { Graze = 0, Walk, Run, Scared, Hit, Die, Idle, Count };
enum class VimpState : int { Graze, Walk, Flee, Hit, Dead };

struct Vimp {
    Vec3 pos, target;
    float yaw = 0;
    VimpState state = VimpState::Graze;
    VimpClip clip = VimpClip::Graze;
    float stateTime = 0, duration = 4, clipTime = 0, rate = 1;
    float hp = 20;
    int herd = 0;
    int slot = -1;        // model slot (game layer)
    bool remove = false;
    uint32_t uid = 0;     // stable id (the list shrinks)
    // Rework (user): killed, the body lies with its soul standing over it; later it is born again by
    // its herd, small, and grows - killable again once grown. eaten: no body (the giant ate it).
    float growth = 1.0f;
    bool eaten = false;
};

struct VimpHerd {
    Vec3 centre;          // where the herd grazes
    float calm = 99;      // seconds since it was last frightened
    Vec3 fleeTo;
};

struct VimpParams {
    float walkSpeed = 1.3f, runSpeed = 7.5f;    // m/s
    float walkStride = 1.3f, runStride = 7.5f;  // m/s the clips cover at 1x (provisional)
    float herdRadius = 14.0f;                    // m: grazing spread round the herd centre
    float fearRadius = 16.0f;                    // m: the player this close startles them
    float shotFear = 45.0f;                      // m: gunfire / blasts this close
    float fleeDistance = 60.0f;                  // m: how far a stampede runs
    float calmAfter = 8.0f;
    float turnRate = 3.0f;
    float corpseTime = 20.0f;                    // s a carcass stays (unused since the rework: see rebirth)
    float rebirthTime = 60.0f;                   // s until a killed Vimp is born again (rework)
    float growTime = 90.0f;                      // s from newborn to grown (rework)
};

class VimpDirector {
public:
    VimpParams params;
    std::vector<Vimp> vimps;
    std::vector<VimpHerd> herds;
    std::function<float(float, float)> groundHeight;
    std::function<bool(float, float)> walkable;  // grass, not sea or cliff (optional)

    int addHerd(const Vec3& centre);
    int add(int herd, const Vec3& pos, float yaw);
    // threats: the player and other frightening things; scares: shots / blasts this frame.
    void update(float dt, const std::vector<Vec3>& threats, const std::vector<Vec3>& scares);
    // Damage; true if it died now.
    bool damage(int i, float amount, const Vec3& from);
    size_t aliveCount() const;
    // Grown and alive: can be hunted (a newborn can't).
    static bool killable(const Vimp& v) { return v.state != VimpState::Dead && v.growth >= 1.0f; }
    void eat(int i);  // taken whole by the giant: no body, reborn later

private:
    uint32_t rng_ = 4242u;
    float rand01() { rng_ = rng_ * 1664525u + 1013904223u; return float(rng_ >> 8) / float(1u << 24); }
    void setClip(Vimp& v, VimpClip c);
    void pickGraze(Vimp& v);
    void stampede(int herd, const Vec3& from);
};

}  // namespace gvr
