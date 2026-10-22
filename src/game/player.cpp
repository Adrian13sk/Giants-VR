#include "game/player.h"

#include <cstdio>
#include <cstdlib>

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
    fuel_ = params.fuelSeconds;
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
    const float speed = swimming_ ? swimSpeed * mag : params.runSpeed * mag * (in.sprint ? params.sprintFactor : 1.0f);
    // Jetpack fuel: drains while thrusting, refills after a short delay.
    thrusting_ = in.jet && fuel_ > 0.0f && jetpackAvailable;
    if (thrusting_) {
        fuel_ = std::max(0.0f, fuel_ - dt);
        sinceThrust_ = 0;
    } else {
        sinceThrust_ += dt;
        if (sinceThrust_ > params.fuelRefillDelay)
            fuel_ = std::min(params.fuelSeconds, fuel_ + params.fuelRefillRate * params.fuelSeconds * dt);
    }
    // Ground steepness under the feet (terrain gradient over +-0.5 m).
    Vec3 downhill{0, 0, 0};
    bool steep = false;
    // Standing on a structure (a bridge deck, a roof): the terrain's slope underneath doesn't
    // matter (bridges span steep gullies; sliding off them was a device-test complaint).
    if (onGround && !thrusting_ && groundHeight && !onObject_ && !swimming_) {
        // One-sided differences, the gentler side per axis: standing on flat ground at the top
        // of a cliff (a bridge's landing) is flat ground, not a 50-degree slope that a centred
        // difference across the edge would report.
        const float e = 0.5f;
        const float h0 = groundHeight(position.x, position.z);
        auto gentler = [](float a, float b) { return std::fabs(a) < std::fabs(b) ? a : b; };
        const float gx = gentler((groundHeight(position.x + e, position.z) - h0) / e, (h0 - groundHeight(position.x - e, position.z)) / e);
        const float gz = gentler((groundHeight(position.x, position.z + e) - h0) / e, (h0 - groundHeight(position.x, position.z - e)) / e);
        const float grade = std::sqrt(gx * gx + gz * gz);
        steep = grade > params.maxSlope;  // steeper than 50 deg
        if (grade > 1e-4f) downhill = Vec3{-gx / grade, 0, -gz / grade};
    }
    sliding_ = steep;
    if (sliding_) {
        // Too steep to stand: skid downhill, accelerating; the stick only steers sideways.
        const float along = velocity.x * downhill.x + velocity.z * downhill.z;
        Vec3 lat{velocity.x - downhill.x * along, 0, velocity.z - downhill.z * along};
        const float newAlong = std::min(params.slideMaxSpeed, std::max(0.0f, along) + params.slideAccel * dt);
        const Vec3 side{-downhill.z, 0, downhill.x};
        const float steer = (dir.x * side.x + dir.z * side.z) * mag;
        lat = lat * 0.98f + side * (steer * params.slideSteer * dt);
        velocity.x = downhill.x * newAlong + lat.x;
        velocity.z = downhill.z * newAlong + lat.z;
        slideCarry_ = true;
    } else if (onGround && !thrusting_) {
        const float tx = dir.x * speed, tz = dir.z * speed;
        if (slideCarry_) {
            // Off the slope: the slide speed bleeds off instead of stopping dead.
            float dx = tx - velocity.x, dz = tz - velocity.z;
            const float l = std::sqrt(dx * dx + dz * dz), step = params.slideFriction * dt;
            if (l <= step) slideCarry_ = false;
            else { dx *= step / l; dz *= step / l; }
            velocity.x += dx;
            velocity.z += dz;
        } else {
            // On foot: instant response (VR comfort).
            velocity.x = tx;
            velocity.z = tz;
        }
    } else {
        // In the air: thrust builds horizontal speed gradually up to the jetpack's limit;
        // without thrust, momentum carries and the stick steers gently.
        const float top = thrusting_ ? params.jetAirSpeed * mag : std::max(speed, 0.0f);
        const float accel = thrusting_ ? params.jetAirAccel : params.airAccel;
        const float tx = dir.x * top, tz = dir.z * top;
        float dx = tx - velocity.x, dz = tz - velocity.z;
        const float step = accel * dt;
        const float l2 = std::sqrt(dx * dx + dz * dz);
        if (l2 > step) { dx *= step / l2; dz *= step / l2; }
        velocity.x += dx;
        velocity.z += dz;
    }
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
    } else if (sliding_) {
        turnToward(std::atan2(-downhill.x, -downhill.z), params.turnRate);  // skid facing downhill
    } else if (speed > 0.05f) {
        turnToward(moveYaw, params.turnRate);
    }

    // --- Vertical: jetpack and gravity.
    if (thrusting_) {
        velocity.y += params.thrust * dt;
        onGround = false;
    }
    velocity.y -= params.gravity * dt;
    velocity.y = std::min(velocity.y, params.maxRise);
    if (onGround && !thrusting_) velocity.y = 0;
    const float prevY = position.y;
    const Vec3 before = position;
    position += velocity * dt;
    float speedNow = slideCarry_ ? std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z) : speed;
    if (collision && collision->pushOut(position, radius, height, stepUp)) {
        // Blocked or sliding along a wall: animate (and report) the motion that happened.
        const float dx = position.x - before.x, dz = position.z - before.z;
        speedNow = std::sqrt(dx * dx + dz * dz) / dt;
        if (speedNow < 1e-3f) { velocity.x = velocity.z = 0; }
        else { velocity.x = dx / dt; velocity.z = dz / dt; }
    }

    bool objectGround = false, waterGround = false;
    auto groundAt = [&](float x, float z) {
        float gg = groundHeight ? groundHeight(x, z) : 0.0f;
        float og;  // walls' tops, roofs, tables: highest one we can step onto or are falling onto
        objectGround = waterGround = false;
        if (collision && collision->groundAt(x, z, std::max(prevY, position.y) + stepUp, og) && og > gg) {
            gg = og;
            objectGround = true;
        }
        // Deep water holds him up: floating, shoulders at the surface.
        if (seaLevel - floatDepth > gg) {
            gg = seaLevel - floatDepth;
            waterGround = true;
            objectGround = false;
        }
        return gg;
    };
    float g = groundAt(position.x, position.z);
    const float hx = position.x - before.x, hz = position.z - before.z;
    const float run = std::sqrt(hx * hx + hz * hz);
    const float rise = g - prevY;
    // Slope test for the terrain only: a structure's edge (a bridge deck's lip, a step) is a step
    // up, limited by maxStepUp, not a 50-degree slope over one frame's travel.
    // Stepping from a structure onto the terrain (off the far end of a bridge) is a step too.
    const bool tooSteep = !objectGround && !onObject_ && !waterGround && rise > 0.02f && run > 1e-4f && rise / run > params.maxSlope;
    if (onGround && !thrusting_ && (rise > params.maxStepUp || tooSteep)) {
        // Cliff face (e.g. climbing out of the sea): blocks like a wall; the jetpack gets you up.
        position.x = before.x;
        position.z = before.z;
        velocity.x = velocity.z = 0;
        speedNow = 0;
        g = groundAt(position.x, position.z);
    }
    onObject_ = objectGround && position.y - g < params.stepDown;
    swimming_ = waterGround && position.y - g < 0.3f;
    if (position.y <= g) {
        if (!onGround && velocity.y < -params.hardLandSpeed) landTimer_ = params.landSeconds;
        position.y = g;
        velocity.y = 0;
        onGround = true;
    } else if (onGround && !thrusting_ && position.y - g < params.stepDown) {
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
    if (!onGround) want = thrusting_ ? PlayerAnim::Thrust : PlayerAnim::Hover;
    else if (swimming_) want = PlayerAnim::Hover;  // treading water (the Meccaryn has no swim clip)
    else if (sliding_) want = PlayerAnim::Slide;
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
