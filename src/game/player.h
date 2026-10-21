// Player character controller (platform- and renderer-independent).
//
// Drives the player Meccaryn: stick movement relative to the viewer's head direction, the body
// turning to face its travel direction, terrain following, a jetpack (thrust against gravity)
// and an animation state machine with cross-fades. Rendering reads the animation outputs.
#pragma once

#include <functional>

#include "core/math.h"
#include "scene/collision.h"

namespace gvr {

enum class PlayerAnim : int { Idle = 0, Run, Thrust, Hover, Land, Strafe, Count };

struct PlayerInput {
    Vec2 move;            // stick, -1..1, +y = forward (deadzone applied by the controller)
    float viewYaw = 0;    // world yaw of the viewer's head (radians, 0 = facing -Z)
    bool jet = false;     // jetpack thrust held
    bool sprint = false;  // sprint held
    bool aiming = false;  // third person: face aimYaw instead of the travel direction
    float aimYaw = 0;
};

struct PlayerParams {
    float runSpeed = 5.0f;        // m/s at full stick deflection (raised after device test 4: "kinda slow")
    float sprintFactor = 1.5f;    // sprint multiplier
    float strideSpeed = 2.3f;     // m/s the run clip covers at 1x playback (measured, mc_run)
    float strafeStride = 2.0f;    // m/s the sidestep clip covers at 1x (measured, mc_sidestep)
    float maxPlayback = 2.4f;     // clip playback rate cap
    float turnRate = 10.0f;       // rad/s body turn toward the travel direction
    float gravity = 14.0f;        // m/s^2
    float thrust = 26.0f;         // m/s^2 upward while the jetpack fires
    float maxRise = 6.0f;         // m/s upward speed cap
    float hardLandSpeed = 3.0f;   // m/s downward speed that triggers the landing clip
    float landSeconds = 0.45f;    // landing clip hold when not moving
    float stepDown = 0.6f;        // m: stay glued to the terrain when walking downhill
    float crossfade = 0.2f;       // s
    float deadzone = 0.2f;
};

class PlayerController {
public:
    PlayerParams params;
    std::function<float(float, float)> groundHeight;  // world (x, z) -> ground y
    // First person: the body faces the view direction (strafe/backpedal clips by movement angle)
    // instead of turning toward the travel direction.
    bool faceView = false;
    const CollisionWorld* collision = nullptr;  // level objects (optional)
    float radius = 0.35f, height = 1.8f, stepUp = 0.45f;  // capsule (m)

    Vec3 position;     // feet, world metres
    float yaw = 0;     // body facing (radians, 0 = -Z, the original models' forward)
    Vec3 velocity;
    bool onGround = true;

    void teleport(const Vec3& p, float facingYaw);
    void update(float dt, const PlayerInput& in);

    // Animation outputs: current clip/time and the clip being faded out.
    PlayerAnim anim() const { return anim_; }
    float animTime() const { return animTime_; }
    PlayerAnim prevAnim() const { return prevAnim_; }
    float prevAnimTime() const { return prevTime_; }
    float fadeWeight() const;  // weight of the CURRENT clip (1 = fully current)
    float horizontalSpeed() const;

private:
    void setAnim(PlayerAnim a);
    PlayerAnim anim_ = PlayerAnim::Idle, prevAnim_ = PlayerAnim::Idle;
    float animTime_ = 0, prevTime_ = 0, fade_ = 1e9f;
    float landTimer_ = 0;
};

// Shortest signed angle from a to b (radians).
float angleDelta(float a, float b);

}  // namespace gvr
