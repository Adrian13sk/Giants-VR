#include "game/vr_rig.h"

#include <cmath>

namespace gvr {

static float applyDeadzone(float v, float dz) {
    float a = std::fabs(v);
    if (a < dz) return 0.0f;
    return (v < 0 ? -1.0f : 1.0f) * (a - dz) / (1.0f - dz);
}

Pose VrRig::toWorld(const Pose& p) const {
    // Scale only the positional part of tracking space (world scale), then map to world.
    Pose scaled = p;
    scaled.position = p.position * comfort.worldScale;
    return worldFromTracking * scaled;
}

void VrRig::update(float dt, const InputFrame& in) {
    const ControllerState& mv = in.hand[moveHand()];
    const ControllerState& tn = in.hand[turnHand()];

    // --- Turning (about the head, so the user does not swing around the play-space centre) ---
    float turnX = applyDeadzone(tn.thumbstick.x, comfort.stickDeadzone);
    float yawDelta = 0.0f;
    if (comfort.snapTurn) {
        if (std::fabs(tn.thumbstick.x) > comfort.snapThreshold) {
            if (snapArmed_) {
                yawDelta = (tn.thumbstick.x > 0 ? -1.0f : 1.0f) * comfort.snapTurnDegrees * kPi / 180.0f;
                snapArmed_ = false;
            }
        } else if (std::fabs(tn.thumbstick.x) < comfort.snapThreshold * 0.5f) {
            snapArmed_ = true;
        }
    } else {
        yawDelta = -turnX * comfort.smoothTurnDegPerSec * kPi / 180.0f * dt;
    }
    if (yawDelta != 0.0f) {
        Vec3 pivot = in.headValid ? toWorld(in.head).position : worldFromTracking.position;
        Quat r = Quat::fromAxisAngle({0, 1, 0}, yawDelta);
        Pose rot{r, pivot - r.rotate(pivot)};
        worldFromTracking = rot * worldFromTracking;
        worldFromTracking.orientation = normalize(worldFromTracking.orientation);
    }

    // --- Smooth locomotion on the horizontal plane ---
    float sx = applyDeadzone(mv.thumbstick.x, comfort.stickDeadzone);
    float sy = applyDeadzone(mv.thumbstick.y, comfort.stickDeadzone);
    if (sx != 0.0f || sy != 0.0f) {
        Quat basis = worldFromTracking.orientation;
        if (comfort.moveRelativeToHead && in.headValid) {
            basis = toWorld(in.head).orientation;
        } else if (mv.aimPoseValid) {
            basis = toWorld(mv.aimPose).orientation;
        }
        float yaw = yawOf(basis);
        Quat yq = Quat::fromAxisAngle({0, 1, 0}, yaw);
        Vec3 local{sx, 0, -sy};
        Vec3 delta = yq.rotate(local) * (comfort.moveSpeed * dt);
        worldFromTracking.position += delta;
    }

    // --- Ground following: keep the tracking-space floor on the terrain under the head ---
    if (groundHeight) {
        Vec3 headW = in.headValid ? toWorld(in.head).position : worldFromTracking.position;
        float g = groundHeight(headW.x, headW.z);
        // Tracking-space floor (y=0) maps to worldFromTracking.position.y (rotation is yaw-only).
        worldFromTracking.position.y = g;
    }
}

void VrRig::teleportHead(const InputFrame& in, const Vec3& worldPos, float worldYaw) {
    float headYaw = in.headValid ? yawOf(in.head.orientation) : 0.0f;
    Quat q = Quat::fromAxisAngle({0, 1, 0}, worldYaw - headYaw);
    Vec3 headT = in.headValid ? in.head.position * comfort.worldScale : Vec3{0, 0, 0};
    headT.y = 0;  // keep the floor at the world height
    worldFromTracking.orientation = q;
    worldFromTracking.position = worldPos - q.rotate(headT);
}

}  // namespace gvr
