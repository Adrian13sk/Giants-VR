// GameApp: VR feel - controller rumble and the holstered weapon.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

// A reference node of the player's skeleton (game frame). Not every clip carries the reference
// nodes (the hover/fly clips have none), so its offset from a bone every clip has is remembered.
bool GameApp::bodyRef(const char* ref, const char* parent, RefCache& c, Mat4& out) {
    if (!playerModel_) return false;
    Mat4 p;
    if (!playerModel_->nodeWorld(parent, p)) return false;
    Mat4 r;
    if (playerModel_->nodeWorld(ref, r)) {
        c.local = affineInverse(p) * r;
        c.have = true;
        out = r;
        return true;
    }
    if (!c.have) return false;
    out = p * c.local;
    return true;
}

void GameApp::updateFeel(float dt, const InputFrame& in) {
    // --- Rumble.
    auto both = [&](float a, float s) {
        haptics_.push_back({kLeft, a, s});
        haptics_.push_back({kRight, a, s});
    };
    const bool thr = player_.thrusting();
    if (thr && !feelThrust_) both(0.55f, 0.1f);  // the jetpack kicks in
    feelRumble_ -= dt;
    if (thr && feelRumble_ <= 0) {               // and hums while it fires (harder on turbo)
        both(player_.turbo() ? 0.3f : 0.13f, 0.12f);
        feelRumble_ = 0.1f;
    }
    feelThrust_ = thr;
    if (!player_.onGround) feelFall_ = std::max(feelFall_, -player_.velocity.y);
    else {
        if (!feelWasGround_ && feelFall_ > 4.0f) both(std::min(1.0f, feelFall_ / 16.0f), 0.06f + std::min(0.12f, feelFall_ * 0.006f));
        feelFall_ = 0;
    }
    feelWasGround_ = player_.onGround;
    // Heartbeat when badly hurt: a double thump, faster the lower he is.
    if (playerHealth_ > 0 && playerHealth_ < 30.0f && !dying_) {
        feelBeat_ -= dt;
        if (feelBeat_ <= 0) {
            both(0.28f, 0.05f);
            feelBeat2_ = 0.17f;
            feelBeat_ = 0.6f + playerHealth_ / 30.0f * 0.5f;
        }
        if (feelBeat2_ > 0) {
            feelBeat2_ -= dt;
            if (feelBeat2_ <= 0) both(0.18f, 0.04f);
        }
    }

    // --- Holsters: the weapon not in hand rides on the body.
    holsterGun_ = holsterLauncher_ = false;
    if (!playerModel_ || !gunModel_) return;
    const bool launcherOut = weaponSel_ == 1 && launcherModel_;
    const bool longOut = launcherOut || gunOfSel(weaponSel_) >= 0;  // a two-handed gun in hand: the pistol rides on the thigh
    const float bodyScale = eyeModel_.y > 0.1f ? userEye_ / eyeModel_.y : 1.0f;
    const float k = firstPerson_ ? bodyScale * 0.6f : 0.75f;  // the in-hand sizes
    const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
    auto worldPoint = [&](const Mat4& g) {
        const Vec4 p = bodyXf_ * Vec4{-g.m[12] * gGameUnitsToMeters, g.m[14] * gGameUnitsToMeters, g.m[13] * gGameUnitsToMeters, 1.0f};
        return Vec3{p.x, p.y, p.z};
    };
    Mat4 hr, hb;
    if (bodyRef("ref_holsterr", "leg r", holsterRCache_, hr)) {
        holsterGunPos_ = worldPoint(hr);
        if (longOut) {
            // Barrel down along the thigh, the grip up by the hip.
            const Quat q = body * rotationBetween(Vec3{0, 0, -1}, normalize(Vec3{0.0f, -1.0f, -0.25f}));
            const Vec3 handle = giantsToRuntime(gunHandle_.x, gunHandle_.y, gunHandle_.z);
            holsterGunXf_ = Mat4::translation(holsterGunPos_ + body.rotate(Vec3{0.06f, 0.05f, 0})) * Mat4::rotation(q) * Mat4::scale({k, k, k}) *
                            Mat4::translation(handle * -1.0f);
            holsterGun_ = true;
        }
    }
    if (bodyRef("ref_holsterb", "spine", holsterBCache_, hb)) {
        holsterBackPos_ = worldPoint(hb);
        if (!launcherOut && hasLauncher_ && launcherModel_) {
            // Slung across the back, muzzle down to the left, below where Timmy rides.
            const Quat q = body * rotationBetween(Vec3{0, 0, -1}, normalize(Vec3{-0.75f, -0.66f, 0.0f}));
            const Vec3 c = (launcherModel_->boundsMin + launcherModel_->boundsMax) * 0.5f;
            holsterLauncherXf_ = Mat4::translation(holsterBackPos_ + body.rotate(Vec3{0, -0.3f * k / 0.75f, 0.12f})) * Mat4::rotation(q) *
                                 Mat4::scale({k, k, k}) * Mat4::translation(c * -1.0f);
            holsterLauncher_ = true;
        }
    }
    // First person: reach for the holstered weapon and grip to draw it (right thigh for the gun,
    // over the shoulder for the launcher); the stick still switches too.
    if (firstPerson_ && hasLauncher_) {
        const int gh = rig_.turnHand();
        const ControllerState& c = in.hand[gh];
        const bool grip = c.squeeze > 0.7f;
        if (grip && !holsterGripHeld_ && c.gripPoseValid) {
            const Vec3 hand = rig_.toWorld(c.gripPose).position;
            const bool atGun = longOut && length(hand - holsterGunPos_) < 0.3f;
            const bool atBack = !launcherOut && (length(hand - holsterBackPos_) < 0.42f ||
                                                 (in.headValid && hand.y > rig_.toWorld(in.head).position.y - 0.1f &&
                                                  dot(hand - rig_.toWorld(in.head).position, body.rotate(Vec3{0, 0, 1})) > 0.02f));
            if (atGun || atBack) {
                weaponSel_ = atGun ? 0 : 1;
                weaponHudT_ = 0;
                sfx2D("ph_selct", 0.7f);
                haptics_.push_back({gh, 0.6f, 0.08f});
                LOGI("Drew the %s from its holster", weaponSel_ ? "rocket launcher" : "gun");
            }
        }
        holsterGripHeld_ = grip;
    }
}

}  // namespace gvr
