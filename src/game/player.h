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

enum class PlayerAnim : int { Idle = 0, Run, Thrust, Hover, Land, Strafe, Slide, Count };

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
    // Jetpack (user description of the original: speed builds up gradually to a limit; limited
    // fuel that refills after a while).
    float thrust = 21.0f;         // m/s^2 upward while the jetpack fires (net +7 against gravity)
    float maxRise = 5.0f;         // m/s upward speed cap
    float jetAirSpeed = 9.0f;     // m/s horizontal top speed while thrusting
    float jetAirAccel = 4.0f;     // m/s^2 horizontal speed build-up while thrusting
    float airAccel = 3.0f;        // m/s^2 air control without thrust (momentum is kept)
    float fuelSeconds = 4.0f;     // full tank = seconds of thrust
    float fuelRefillDelay = 1.2f; // s without thrust before refilling
    float fuelRefillRate = 0.5f;  // tanks per second
    float hardLandSpeed = 3.0f;   // m/s downward speed that triggers the landing clip
    float landSeconds = 0.45f;    // landing clip hold when not moving
    float stepDown = 0.6f;        // m: stay glued to the terrain when walking downhill
    float maxStepUp = 0.8f;       // m: a bigger ground rise in one step is a cliff (blocks)
    float maxSlope = 1.19f;       // rise/run: steeper uphill ground blocks walking (50 deg, the original's gate)
    float slideAccel = 9.0f;      // m/s^2 downhill acceleration on ground steeper than 50 deg
    float slideMaxSpeed = 10.0f;  // m/s
    float slideSteer = 2.0f;      // m/s^2 sideways steering while sliding
    float slideFriction = 12.0f;  // m/s^2 decay of slide speed once back on walkable ground
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
    float fuel() const { return fuel_ / params.fuelSeconds; }  // 0..1
    bool sliding() const { return sliding_; }
    bool thrusting() const { return thrusting_; }

private:
    void setAnim(PlayerAnim a);
    PlayerAnim anim_ = PlayerAnim::Idle, prevAnim_ = PlayerAnim::Idle;
    float animTime_ = 0, prevTime_ = 0, fade_ = 1e9f;
    float landTimer_ = 0;
    float fuel_ = 4.0f, sinceThrust_ = 99.0f;
    bool thrusting_ = false;
    bool sliding_ = false;      // on ground too steep to stand on: skidding downhill
    bool slideCarry_ = false;   // still carrying slide momentum on walkable ground
};

// Shortest signed angle from a to b (radians).
float angleDelta(float a, float b);

}  // namespace gvr
