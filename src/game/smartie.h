// Mission 1b ("The Wax Smarties"): Smarties stranded on an islet must be carried to the Smartie
// pad (sm_dropoff). Their mission records carry trigger 3 ("destination arrival", decoded from
// the mission data); how the player picks them up is ours, designed for VR: reach for a Smartie
// with either hand and squeeze the grip to lift it, release the grip to let go.
//
// Behaviour (ours, the original's is native code): a released Smartie falls under gravity.
// Landing on the pad saves it (the objective); landing in the water (piranhas, per the mission
// text) sends it back to where it was stranded after a moment; landing elsewhere leaves it there.
// Platform independent and unit tested (no rendering).
#pragma once

#include <functional>
#include <vector>

#include "core/math.h"

namespace gvr {

enum class SmartieState { Idle, Carried, Falling, Saved, Drowning };

enum class SmartieClip { Stand, Wave, Squirm, Fall, Celebrate, Drowned, Count };

struct RescueSmartie {
    Vec3 home;            // where it was stranded (returns here after falling in the water)
    Vec3 pos;             // feet, runtime metres
    Vec3 vel;
    float yaw = 0;
    float height = 1.2f;  // body height (m), for reach tests and carrying
    SmartieState state = SmartieState::Idle;
    int hand = -1;        // carrying hand
    float stateTime = 0;
    int objective = -1;   // mission objective completed when saved
    int slot = -1;        // GameApp character slot (rendering)
    SmartieClip clip = SmartieClip::Stand;
};

struct SmartieEvent {
    enum Kind { Grabbed, Released, Saved, Splashed, Returned, Landed } kind;
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
    // a hand may be (the "reach down" assist: nobody should have to touch the floor in VR).
    float reach = 0.9f, reachAbove = 1.6f;

    int add(const Vec3& feet, float yaw, float height);
    // hands: world positions; grip: squeeze 0..1. Appends events (not cleared).
    void update(float dt, const Vec3 hands[2], const float grip[2], const Vec3& playerPos, std::vector<SmartieEvent>& events);
    size_t savedCount() const;
    bool holding(int hand) const;

private:
    bool inPad(const Vec3& p) const;
    bool gripHeld_[2] = {false, false};
    Vec3 prevHand_[2];
    bool havePrev_ = false;
};

}  // namespace gvr
