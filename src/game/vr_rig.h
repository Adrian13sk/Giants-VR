// Player rig: maps the tracking space (where the headset and controllers live) into the game
// world, and applies artificial locomotion (smooth move, snap/smooth turn) with comfort options.
#pragma once

#include <functional>

#include "core/math.h"
#include "game/input_state.h"

namespace gvr {

struct ComfortSettings {
    bool snapTurn = true;              // default for VR newcomers
    float snapTurnDegrees = 30.0f;
    float smoothTurnDegPerSec = 120.0f;
    float moveSpeed = 3.0f;            // metres per second at full stick deflection
    float stickDeadzone = 0.20f;
    float snapThreshold = 0.70f;       // stick deflection that triggers a snap turn
    bool leftHanded = false;           // swaps move/turn sticks and primary/secondary hands
    bool moveRelativeToHead = true;    // false: relative to the off-hand controller
    float worldScale = 1.0f;           // >1 makes the player feel bigger (e.g. Kabuto)
    // Debug fly / noclip mode (temporary map-exploration aid).
    float flySpeed = 15.0f;            // metres per second at full deflection while flying
    float boostMultiplier = 5.0f;      // move-hand grip held: speed x this (walking or flying)
};

class VrRig {
public:
    // worldFromTracking maps tracking-space poses to world space.
    Pose worldFromTracking;
    ComfortSettings comfort;
    // Optional ground query: returns world-space ground height at (x, z).
    std::function<float(float, float)> groundHeight;
    // Debug fly/noclip: move along the full head direction (pitch included), turn-stick Y
    // rises/descends, no ground following or collision.
    bool flyMode = false;

    void update(float dt, const InputFrame& in);
    // Yaw change requested by the turn stick this frame (snap or smooth, per comfort settings).
    float turnInput(float dt, const InputFrame& in);
    // Place the player so that the head is above `worldPos`, facing `worldYaw` (radians).
    void teleportHead(const InputFrame& in, const Vec3& worldPos, float worldYaw);

    Pose toWorld(const Pose& trackingPose) const;
    int moveHand() const { return comfort.leftHanded ? kRight : kLeft; }
    int turnHand() const { return comfort.leftHanded ? kLeft : kRight; }

private:
    bool snapArmed_ = true;
};

}  // namespace gvr
