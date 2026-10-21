// Device-independent input snapshot produced by the platform layer (OpenXR on Quest,
// scripted input in the headless harness) and consumed by gameplay / VR rig code.
#pragma once

#include <cstdint>

#include "core/math.h"

namespace gvr {

enum Hand : int { kLeft = 0, kRight = 1 };

struct ControllerState {
    bool active = false;       // controller connected and its pose tracked this frame
    bool gripPoseValid = false;
    bool aimPoseValid = false;
    Pose gripPose;             // in tracking (stage) space
    Pose aimPose;              // in tracking (stage) space
    float trigger = 0.0f;      // 0..1
    float squeeze = 0.0f;      // 0..1
    Vec2 thumbstick;           // -1..1, +y = forward
    bool primary = false;      // A (right) / X (left)
    bool secondary = false;    // B (right) / Y (left)
    bool thumbstickClick = false;
    bool menu = false;         // left controller menu button
};

struct InputFrame {
    ControllerState hand[2];
    Pose head;                 // tracking space
    bool headValid = false;
    uint32_t recenterSerial = 0;  // increments when the user recentres (reference space change)
};

// Edge detection helper for boolean buttons.
struct ButtonEdge {
    bool prev = false;
    bool pressed(bool now) { bool p = now && !prev; prev = now; return p; }
};

}  // namespace gvr
