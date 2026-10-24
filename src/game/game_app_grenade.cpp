// Grenades (weapondefs item 8, IconGrenade; object mc_grenade_l0, weapon 2: 80 damage, a 30-50 unit blast).
// 5a and kstory5_5 hand them out. Ours, made for VR: pick them on the weapon strip; one sits in your
// hand; hold the trigger (the pin is out), swing and let go - it flies with your hand's speed.
// Let go without a throw and it is lobbed ahead (so third person and a gentle flick still work).
// It bounces and bursts when its 2.5 s fuse runs out, or on whoever it strikes (updateRockets).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "scene/model_cache.h"

namespace gvr {

void GameApp::updateGrenades(float dt, const InputFrame& in) {
    const int gh = rig_.turnHand();
    const ControllerState& c = in.hand[gh];
    // The throwing hand's velocity (world), smoothed over a few frames.
    if (c.gripPoseValid && dt > 1e-4f) {
        const Vec3 p = rig_.toWorld(c.gripPose).position;
        if (grenHandValid_) {
            grenHandVel_ = grenHandVel_ + ((p - grenHandPrev_) * (1.0f / dt) - grenHandVel_) * 0.5f;
            grenVelHist_[grenVelAt_] = grenHandVel_;
            grenVelAt_ = (grenVelAt_ + 1) % 10;
        }
        grenHandPrev_ = p;
        grenHandValid_ = true;
    } else {
        grenHandValid_ = false;
    }
    if (weaponSel_ == 8 && grenades_ <= 0) weaponSel_ = 0;
    const bool held = c.trigger > 0.6f;
    if (!playMode_ || dying_ || hero_ != Hero::Mecc || weaponSel_ != 8 || grenades_ <= 0) { grenCocked_ = false; return; }
    if (held && !grenCocked_) {
        grenCocked_ = true;
        sfx2D("ph_selct", 0.6f, 1.4f);  // the pin
        haptics_.push_back({gh, 0.3f, 0.04f});
    }
    if (!held && grenCocked_) {
        grenCocked_ = false;
        Vec3 from = player_.position + Vec3{0, 1.4f, 0};
        if (firstPerson_ && c.gripPoseValid) from = rig_.toWorld(c.gripPose).position;
        const float yaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : player_.yaw;
        Vec3 aim{-std::sin(yaw), 0, -std::cos(yaw)};
        if (firstPerson_ && c.aimPoseValid) aim = rig_.toWorld(c.aimPose).orientation.rotate(Vec3{0, 0, -1});
        // The fastest of the last ten frames: players let go as the swing slows down.
        Vec3 best = grenHandVel_;
        for (const auto& h : grenVelHist_)
            if (length(h) > length(best)) best = h;
        for (auto& h : grenVelHist_) h = {0, 0, 0};
        Vec3 v = best * 1.6f;  // a little help: arms in VR don't follow through like a real throw
        const float sp = length(v);
        if (!firstPerson_ || sp < 3.0f) v = normalize(Vec3{aim.x, std::max(aim.y, 0.0f) + 0.45f, aim.z}) * 12.0f;  // lobbed ahead
        else if (sp > 30.0f) v = v * (30.0f / sp);
        v = v + player_.velocity * 0.5f;
        Rocket r{from, v, 0.0f, 6};
        rockets_.push_back(r);
        --grenades_;
        sfx("me_grnth", from, 0.9f, 1.0f);
        haptics_.push_back({gh, 0.6f, 0.08f});
        LOGI("Grenade thrown at %.1f m/s (%d left)", length(v), grenades_);
        if (grenades_ <= 0) weaponSel_ = 0;
    }
}

void GameApp::renderGrenades() {
    // One in the hand while grenades are chosen (first person: the controller's grip).
    if (!grenadeModel_ || !playMode_ || dying_ || weaponSel_ != 8 || grenades_ <= 0 || !firstPerson_) return;
    const ControllerState& c = lastInput_.hand[rig_.turnHand()];
    if (!c.gripPoseValid) return;
    const Pose hand = rig_.toWorld(c.gripPose);
    const float k = 0.45f;  // the model is ~0.25 m across: a 10 cm grenade, just ahead of the palm
    drawModel(grenadeModel_, Mat4::translation(hand.position + hand.orientation.rotate(Vec3{0, 0.0f, -0.09f})) * Mat4::rotation(hand.orientation) *
                                 Mat4::scale({k, k, k}) * Mat4::translation((grenadeModel_->boundsMin + grenadeModel_->boundsMax) * -0.5f),
              false);
}

}  // namespace gvr
