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
    const float speed = params.runSpeed * mag;
    velocity.x = dir.x * speed;
    velocity.z = dir.z * speed;
    if (speed > 0.05f) {
        const float target = std::atan2(-dir.x, -dir.z);
        const float d = angleDelta(yaw, target);
        const float step = params.turnRate * dt;
        yaw += std::fabs(d) <= step ? d : (d > 0 ? step : -step);
        yaw = angleDelta(0.0f, yaw);  // keep in (-pi, pi]
    }

    // --- Vertical: jetpack and gravity.
    if (in.jet) {
        velocity.y += params.thrust * dt;
        onGround = false;
    }
    velocity.y -= params.gravity * dt;
    velocity.y = std::min(velocity.y, params.maxRise);
    if (onGround && !in.jet) velocity.y = 0;
    position += velocity * dt;

    const float g = groundHeight ? groundHeight(position.x, position.z) : 0.0f;
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
    if (!onGround) want = in.jet ? PlayerAnim::Thrust : PlayerAnim::Hover;
    else if (speed > 0.15f) want = PlayerAnim::Run;
    else if (landTimer_ > 0) want = PlayerAnim::Land;
    else want = PlayerAnim::Idle;
    setAnim(want);
    // Run playback follows ground speed so the feet do not slide.
    float rate = 1.0f;
    if (anim_ == PlayerAnim::Run) rate = std::max(0.6f, std::min(1.6f, speed / params.strideSpeed));
    animTime_ += dt * rate;
    prevTime_ += dt;
    fade_ += dt;
}

}  // namespace gvr
