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

enum class PlayerAnim : int { Idle = 0, Run, Thrust, Hover, Land, Strafe, Slide, FlyFwd, FlyLeft, FlyRight, FlyBack, LandHard, Fall, Inject, Count };

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
    // Jetpack, as in the original (Speed Demos Archive notes and the user's description): the
    // Meccaryn steers in the air only while the jetpack fires; without it air friction slows him
    // and he falls. The tank refills only on the ground; burning it dry overheats the jetpack, and
    // it won't fire again until the tank is partly refilled. A hard landing stuns him for 2 s and
    // hurts. Turbo (sprint while flying) is faster and burns fuel twice as fast. Numbers ours.
    float thrust = 22.0f;         // m/s^2 upward while the jetpack fires (net +8 against gravity)
    float maxRise = 6.0f;         // m/s upward speed cap
    float jetAirSpeed = 10.0f;    // m/s horizontal flight speed
    float turboFactor = 1.5f;     // turbo: faster, double fuel use
    float jetAirAccel = 9.0f;     // m/s^2 horizontal speed build-up while flying
    float airDrag = 0.6f;         // 1/s horizontal slow-down in the air without thrust
    float fuelSeconds = 5.0f;     // full tank = seconds of thrust
    float fuelRefillDelay = 0.3f; // s on the ground without thrust before refilling
    float fuelRefillRate = 0.4f;  // tanks per second (on the ground)
    float overheatResume = 0.3f;  // after burning dry: usable again at this much fuel
    float stunSpeed = 12.0f;      // m/s impact: hard landing (stun 2 s, fall damage)
    float stunSeconds = 2.0f;
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
    // False until the jetpack is in the inventory (Mecc story 1 starts without it: Baz loses it
    // in the opening cutscene and gets it from Timmy).
    bool jetpackAvailable = true;
    // Water: deeper than floatDepth the Meccaryn floats with his shoulders at the surface (he
    // can't dive) and swims at swimSpeed. seaLevel -1e9 = no water.
    float seaLevel = -1e9f, floatDepth = 1.35f, swimSpeed = 2.4f;
    bool swimming() const { return swimming_; }
    bool onObject() const { return onObject_; }  // standing on a structure (bridge, walkway, roof)
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
    bool overheated() const { return overheated_; }
    bool turbo() const { return turbo_; }
    bool stunned() const { return stun_ > 0; }
    // A one-shot body action (the health injection): plays `a` for `seconds` while standing
    // (walking slowed); the clip stays on screen until it ends or the player leaves the ground.
    void startAction(PlayerAnim a, float seconds) { action_ = a; actionTime_ = seconds; }
    bool inAction() const { return actionTime_ > 0; }
    // Downward speed (m/s) of a landing that happened in the last update, once (0 = none).
    float takeImpact() { const float v = impact_; impact_ = 0; return v; }
    // True once when the jetpack has just burnt dry.
    bool takeOverheat() { const bool o = overheatEvent_; overheatEvent_ = false; return o; }

private:
    void setAnim(PlayerAnim a);
    PlayerAnim anim_ = PlayerAnim::Idle, prevAnim_ = PlayerAnim::Idle;
    float animTime_ = 0, prevTime_ = 0, fade_ = 1e9f;
    float landTimer_ = 0;
    float fuel_ = 5.0f, sinceThrust_ = 99.0f;
    bool thrusting_ = false, overheated_ = false, overheatEvent_ = false, turbo_ = false;
    float stun_ = 0, impact_ = 0;
    PlayerAnim action_ = PlayerAnim::Idle;
    float actionTime_ = 0;
    bool sliding_ = false;      // on ground too steep to stand on: skidding downhill
    bool slideCarry_ = false;  // still carrying slide momentum on walkable ground
    bool onObject_ = false;  // feet on a structure (bridge, roof), not the terrain
    bool swimming_ = false;  // floating in deep water
};

// Shortest signed angle from a to b (radians).
float angleDelta(float a, float b);

}  // namespace gvr
