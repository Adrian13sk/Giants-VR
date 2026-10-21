#include "game/player.h"

#include <algorithm>
#include <cmath>

namespace gvr {

float angleDelta(float a, float b) {
    float d = std::fmod(b - a + kPi, 2.0f * kPi);
    if (d < 0) d += 2.0f * kPi;
    return d - kPi;
}

static float deadzone(float v, float dz) {
    const float a = std::fabs(v);
    if (a < dz) return 0.0f;
    return (v < 0 ? -1.0f : 1.0f) * (a - dz) / (1.0f - dz);
}

void PlayerController::teleport(const Vec3& p, float facingYaw) {
    position = p;
    if (groundHeight) position.y = groundHeight(p.x, p.z);
    yaw = facingYaw;
    velocity = {0, 0, 0};
    onGround = true;
    anim_ = prevAnim_ = PlayerAnim::Idle;
    animTime_ = prevTime_ = 0;
    fade_ = 1e9f;
}

float PlayerController::horizontalSpeed() const {
    return std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
}

float PlayerController::fadeWeight() const {
    return params.crossfade > 0 ? std::min(1.0f, fade_ / params.crossfade) : 1.0f;
}

void PlayerController::setAnim(PlayerAnim a) {
    if (a == anim_) return;
    prevAnim_ = anim_;
    prevTime_ = animTime_;
    anim_ = a;
    animTime_ = 0;
    fade_ = 0;
}

void PlayerController::update(float dt, const PlayerInput& in) {
    if (dt <= 0) return;
    // --- Horizontal: stick relative to where the viewer looks; instant response (VR comfort).
    float sx = deadzone(in.move.x, params.deadzone), sy = deadzone(in.move.y, params.deadzone);
    const float mag = std::min(1.0f, std::sqrt(sx * sx + sy * sy));
    const float s = std::sin(in.viewYaw), c = std::cos(in.viewYaw);
    const Vec3 fwd{-s, 0, -c}, right{c, 0, -s};
    Vec3 dir = fwd * sy + right * sx;
    const float dl = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    if (dl > 1e-4f) dir = dir * (1.0f / dl);
    const float speed = params.runSpeed * mag * (in.sprint ? params.sprintFactor : 1.0f);
    velocity.x = dir.x * speed;
    velocity.z = dir.z * speed;
    const float moveYaw = std::atan2(-dir.x, -dir.z);
    auto turnToward = [&](float target, float rate) {
        const float d = angleDelta(yaw, target);
        const float step = rate * dt;
        yaw += std::fabs(d) <= step ? d : (d > 0 ? step : -step);
        yaw = angleDelta(0.0f, yaw);  // keep in (-pi, pi]
    };
    if (faceView) {
        // Body follows the view smoothly (exponential, no snapping). Moving: tight follow.
        // Standing: the head may look up to ~30 deg off the body freely; beyond that the body
        // is carried along smoothly, and it slowly re-centres under the view.
        const float d = angleDelta(yaw, in.viewYaw);
        const auto follow = [&](float amount, float tau) { yaw += amount * (1.0f - std::exp(-dt / tau)); };
        if (speed > 0.05f) {
            follow(d, 0.10f);
        } else {
            const float cone = 0.52f;
            if (std::fabs(d) > cone) follow(d - (d > 0 ? cone : -cone), 0.12f);
            follow(angleDelta(yaw, in.viewYaw), 1.5f);
        }
        yaw = angleDelta(0.0f, yaw);
    } else if (in.aiming) {
        turnToward(in.aimYaw, params.turnRate * 1.5f);  // strafe/backpedal clips handle travel
    } else if (speed > 0.05f) {
        turnToward(moveYaw, params.turnRate);
    }

    // --- Vertical: jetpack and gravity.
    if (in.jet) {
        velocity.y += params.thrust * dt;
        onGround = false;
    }
    velocity.y -= params.gravity * dt;
    velocity.y = std::min(velocity.y, params.maxRise);
    if (onGround && !in.jet) velocity.y = 0;
    const float prevY = position.y;
    const Vec3 before = position;
    position += velocity * dt;
    float speedNow = speed;
    if (collision && collision->pushOut(position, radius, height, stepUp)) {
        // Blocked or sliding along a wall: animate (and report) the motion that happened.
        const float dx = position.x - before.x, dz = position.z - before.z;
        speedNow = std::sqrt(dx * dx + dz * dz) / dt;
        if (speedNow < 1e-3f) { velocity.x = velocity.z = 0; }
        else { velocity.x = dx / dt; velocity.z = dz / dt; }
    }

    auto groundAt = [&](float x, float z) {
        float gg = groundHeight ? groundHeight(x, z) : 0.0f;
        float og;  // walls' tops, roofs, tables: highest one we can step onto or are falling onto
        if (collision && collision->groundAt(x, z, std::max(prevY, position.y) + stepUp, og)) gg = std::max(gg, og);
        return gg;
    };
    float g = groundAt(position.x, position.z);
    if (onGround && !in.jet && g - prevY > params.maxStepUp) {
        // Cliff face (e.g. climbing out of the sea): blocks like a wall; the jetpack gets you up.
        position.x = before.x;
        position.z = before.z;
        velocity.x = velocity.z = 0;
        speedNow = 0;
        g = groundAt(position.x, position.z);
    }
    if (position.y <= g) {
        if (!onGround && velocity.y < -params.hardLandSpeed) landTimer_ = params.landSeconds;
        position.y = g;
        velocity.y = 0;
        onGround = true;
    } else if (onGround && !in.jet && position.y - g < params.stepDown) {
        position.y = g;  // walking downhill: stay on the terrain
    } else if (position.y - g >= params.stepDown) {
        onGround = false;  // ran off a ledge
    }

    // --- Animation state machine.
    landTimer_ = std::max(0.0f, landTimer_ - dt);
    PlayerAnim want;
    float rate = 1.0f;
    // Movement relative to the body: forward run, sidestep (right = clip forward), backpedal.
    const float rel = angleDelta(yaw, moveYaw);
    if (!onGround) want = in.jet ? PlayerAnim::Thrust : PlayerAnim::Hover;
    else if (speedNow > 0.15f) {
        const float clampRate = params.maxPlayback;
        if (std::fabs(rel) < 0.87f) {           // within ~50 deg of facing
            want = PlayerAnim::Run;
            rate = std::min(clampRate, std::max(0.6f, speedNow / params.strideSpeed));
        } else if (std::fabs(rel) > 2.27f) {    // beyond ~130 deg: run the cycle backwards
            want = PlayerAnim::Run;
            rate = -std::min(clampRate, std::max(0.6f, speedNow / params.strideSpeed));
        } else {
            want = PlayerAnim::Strafe;
            rate = std::min(clampRate, std::max(0.6f, speedNow / params.strafeStride)) * (rel < 0 ? 1.0f : -1.0f);
        }
    } else if (landTimer_ > 0) want = PlayerAnim::Land;
    else want = PlayerAnim::Idle;
    setAnim(want);
    animTime_ += dt * rate;
    prevTime_ += dt;
    fade_ += dt;
}

}  // namespace gvr
