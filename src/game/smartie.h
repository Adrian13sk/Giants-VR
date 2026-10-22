// Mission 1b ("The Wax Smarties"): Smarties hang from the rock pillars (hang points from the
// pillars' own ref_smartie_NN nodes) and must be carried to the Smartie pad (sm_dropoff). Their
// mission records carry trigger 3 ("destination arrival", decoded from the mission data).
//
// Behaviour (ours, the original's is native code), as described by the user from the original:
// each Smartie can only hang on for a while; when its grip gives out it falls into the water,
// where the piranhas get it; some time later another one hangs there again, so the mission can
// still be finished. Picking up is designed for VR: reach for a Smartie with either hand and
// squeeze the grip; release the grip to let go. A released Smartie falls under gravity: on the
// pad it is saved (the objective), in the water it dies, on land it stands and waits.
// Platform independent and unit tested (no rendering).
#pragma once

#include <functional>
#include <vector>

#include "core/math.h"

namespace gvr {

enum class SmartieState { Hanging, Idle, Carried, Falling, Saved, Drowning, Gone };

enum class SmartieClip { Stand, Wave, Squirm, Fall, Celebrate, Drowned, Hang, HangOneArm, Count };

struct RescueSmartie {
    Vec3 home;            // where it waits when not hanging (mission position)
    Vec3 hangPoint;       // hands' position when hanging
    bool hangs = false;   // has a hang point (starts hanging, respawns hanging)
    Vec3 pos;             // feet, runtime metres
    Vec3 vel;
    float yaw = 0;
    float height = 1.2f;  // body height (m), for reach tests and carrying
    SmartieState state = SmartieState::Idle;
    int hand = -1;        // carrying hand
    float stateTime = 0;
    float holdFor = 60;   // seconds it can hang on this time
    int objective = -1;   // mission objective completed when saved
    int slot = -1;        // GameApp character slot (rendering)
    SmartieClip clip = SmartieClip::Stand;
    bool visible() const { return state != SmartieState::Gone; }
};

struct SmartieEvent {
    enum Kind { Grabbed, Released, Saved, Splashed, Returned, Landed, Slipping, LetGo, Died } kind;
    int index;
    int hand;
    Vec3 pos;
};

class RescueDirector {
public:
    std::vector<RescueSmartie> smarties;
    Vec3 padMin, padMax;             // drop-off zone (world bounds of the pad, padded)
    bool hasPad = false;
    float seaLevel = 0.0f;           // water surface height
    std::function<float(float, float)> groundHeight;  // terrain/sea floor height at (x, z)
    // Reach: horizontal distance from the hand to the Smartie's axis, and how far above its head
    // a hand may be (the "reach down" assist for Smarties on the ground: nobody should have to
    // touch the floor in VR). Hanging Smarties are grabbed anywhere along the body.
    float reach = 0.9f, reachAbove = 1.6f;
    // Hanging: how long a Smartie holds on (random in [min, max]); it shows it is slipping (one
    // arm) for the last `warnTime` seconds; a lost one hangs again after `respawnTime`.
    float holdMin = 70.0f, holdMax = 120.0f, warnTime = 12.0f, respawnTime = 25.0f;

    int add(const Vec3& feet, float yaw, float height);
    void setHang(int index, const Vec3& hands);  // starts it hanging there
    // hands: world positions; grip: squeeze 0..1. Appends events (not cleared).
    void update(float dt, const Vec3 hands[2], const float grip[2], const Vec3& playerPos, std::vector<SmartieEvent>& events);
    size_t savedCount() const;
    bool holding(int hand) const;

private:
    bool inPad(const Vec3& p) const;
    void hangAgain(RescueSmartie& s);
    float rnd();
    bool gripHeld_[2] = {false, false};
    Vec3 prevHand_[2];
    bool havePrev_ = false;
    unsigned rng_ = 12345u;
};

}  // namespace gvr
