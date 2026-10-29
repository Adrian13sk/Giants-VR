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
    // Capacitive touch (2026-10-28; Touch controllers): a finger on the trigger, the thumb on a button,
    // the stick or the thumb rest. touchValid: the runtime reports them (else both read as touching).
    bool touchValid = false;
    bool triggerTouch = true;
    bool thumbTouch = true;
    // (2026-10-28) A bare hand, not a controller (hand tracking): only the aim ray, the index pinch as `trigger`
    // and the left hand's menu gesture as `menu` are filled; the platform passes them on only for the menus.
    bool handTracked = false;
};

struct InputFrame {
    ControllerState hand[2];
    Pose head;                 // tracking space
    bool headValid = false;
    uint32_t recenterSerial = 0;  // increments when the user recentres (reference space change)
    bool focused = true;          // (2026-10-29) false while the system menu / Guardian has input focus (the game pauses)
};

// Edge detection helper for boolean buttons.
struct ButtonEdge {
    bool prev = false;
    bool pressed(bool now) { bool p = now && !prev; prev = now; return p; }
};

}  // namespace gvr
