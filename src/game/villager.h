// Smartie villagers: Smarties living in a village (Smartie houses), going about their day -
// standing about, chatting, laughing, singing, tai chi, sitting, strolling between the houses -
// waving at the Meccaryn when he comes close, running from danger (Reapers, Rippers, gunfire,
// blasts) and cowering until it passes. Renderer-independent; the game layer poses the original
// sm_* clips named by VillagerClip. The behaviour is ours: the original Story 1 missions place no
// villagers (user request, device test 24 list).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/math.h"

namespace gvr {

enum class VillagerClip : int {
    Stand = 0, Walk, Scan, Talk, Listen, Laugh, Sing, TaiChi, Sit, Celebrate, Wave, Run, Cower,
    GetsShot, Death, Squirm, HandsUp, Count  // (the last four: rework - hit, killed, held, a gun pointed at them)
};

enum class VillagerState : int { Idle, Walk, Act, Wave, Flee, Cower, Hurt, Dead, Held, HandsUp };

struct Villager {
    Vec3 pos, home, target;
    float yaw = 0;
    VillagerState state = VillagerState::Idle;
    VillagerClip clip = VillagerClip::Stand;
    float stateTime = 0, duration = 2, clipTime = 0, rate = 1;
    float calm = 99;       // seconds since the last fright
    float waveCooldown = 0;
    int slot = -1;         // model slot (game layer)
    uint32_t uid = 0;      // stable id (the list shrinks)
    float hp = 30;         // rework: they can be hurt and killed (by any hero)
    Vec3 fleeFrom;         // rework: where the hurt came from
};

struct VillagerParams {
    float walkSpeed = 1.1f;     // m/s strolling
    float runSpeed = 3.6f;      // m/s fleeing
    float walkStride = 1.1f;    // m/s the walk clip covers at 1x (provisional)
    float runStride = 3.0f;
    float homeRadius = 16.0f;   // m: strolls stay this close to home
    float fearRadius = 22.0f;   // m: an enemy this close scares them
    float blastFear = 30.0f;    // m: an explosion or gunfire this close scares them
    float waveRadius = 5.0f;    // m: the Meccaryn this close (and calm): they wave
    float calmAfter = 6.0f;     // s without a threat before they stop cowering
    float turnRate = 6.0f;      // rad/s
    float corpseTime = 120.0f;  // s a body lies before it goes
};

class VillagerDirector {
public:
    VillagerParams params;
    std::vector<Villager> villagers;
    std::function<float(float, float)> groundHeight;           // world (x, z) -> ground y
    std::function<bool(const Vec3&, const Vec3&)> blocked;     // walking a -> b runs into something

    int add(const Vec3& pos, float yaw);
    // threats: positions of enemies about; scares: explosions / gunfire this frame.
    void update(float dt, const Vec3& player, const std::vector<Vec3>& threats, const std::vector<Vec3>& scares);
    // The player waved at them: the calm ones within `radius` wave back (rework). Returns how many.
    int waveBack(const Vec3& player, float radius);
    // The danger has passed: the calm ones celebrate (sm_celebrate). Returns how many.
    int cheer();
    // Rework: hurt / killed (returns true if this killed it); held in a hand (squirming) or let go;
    // hands up (a gun pointed at it).
    bool damage(int i, float amount, const Vec3& from);
    void hold(int i, bool held);
    void handsUp(int i);
    float clipDuration[int(VillagerClip::Count)] = {};  // (set by the game layer: one-shot clips hold their last frame)
    static bool alive(const Villager& v) { return v.state != VillagerState::Dead; }
    // Pick a random action (exposed for tests).
    void startIdle(Villager& v);

private:
    uint32_t rng_ = 12345u;
    float rand01() { rng_ = rng_ * 1664525u + 1013904223u; return float(rng_ >> 8) / float(1u << 24); }
    void setClip(Villager& v, VillagerClip c);
    void face(Villager& v, const Vec3& dir, float dt);
};

}  // namespace gvr
