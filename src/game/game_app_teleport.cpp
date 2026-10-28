// Teleport movement (2026-10-28; VR-only, not in the original) - Options -> MOVEMENT: TELEPORT, for players
// whom smooth stick walking makes queasy. Push the move stick forward: an arc leaves the move hand's controller
// and lands on the ground ahead (green where you may stand, red where you may not: too steep, deep water, out of
// reach); let the stick go and you blink there - a short fade to black and back. On foot only: in the air (the
// jetpack) and on the Reaperski the stick moves you as before, and races keep smooth movement. Turning, your real
// steps and everything else are unchanged.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace gvr {

bool GameApp::teleportMode() const {
    return settings_.teleport && playMode_ && !riding_ && !raceMode_ && !dying_ && !cutsceneActive() && !menuOpen_;
}

void GameApp::updateTeleport(float dt, PlayerInput& pi, const InputFrame& in) {
    // The blink: dark in 0.08 s, moved, light again in 0.15 s.
    if (telPhase_ == 1) {
        telFade_ = std::min(1.0f, telFade_ + dt / 0.08f);
        if (telFade_ >= 1.0f) {
            const float before = length(telPending_ - player_.position);
            player_.teleport(telPending_, player_.yaw);
            placeCamera(0.0f, true, &in);  // (the camera snaps with it; not teleportPlayer, which would reset the view yaw)
            telPhase_ = 2;
            LOGI("Teleport: %.1f m", before);
        }
    } else if (telPhase_ == 2) {
        telFade_ = std::max(0.0f, telFade_ - dt / 0.15f);
        if (telFade_ <= 0.0f) telPhase_ = 0;
    }
    telArc_.clear();
    if (!teleportMode() || !player_.onGround) { telAiming_ = false; return; }
    const int mh = rig_.moveHand();
    const ControllerState& c = in.hand[mh];
    const Vec2 st = c.thumbstick;
    pi.move = Vec2{0, 0};  // (the stick aims; it doesn't walk)
    pi.sprint = false;
    const bool push = st.y > 0.6f;
    if (push && c.aimPoseValid && telPhase_ == 0) {
        telAiming_ = true;
        // The arc: thrown from the controller, scaled with the hero (Kabuto's reach is a giant's).
        const float s = std::max(1.0f, heroScale_);
        const Pose aim = rig_.toWorld(c.aimPose);
        Vec3 p = aim.position, v = aim.orientation.rotate(Vec3{0, 0, -1}) * (9.0f * std::sqrt(s));
        const float g = 9.8f * std::sqrt(s), step = 0.025f;
        telValid_ = false;
        bool landed = false;
        telArc_.push_back(p);
        for (int i = 0; i < 100 && !landed; ++i) {
            const Vec3 q = p + v * step + Vec3{0, -0.5f * g * step * step, 0};
            v.y -= g * step;
            const Vec3 d = q - p;
            const float len = length(d);
            float t = 0;
            Vec3 n;
            if (worldRay_ && len > 1e-4f && worldRay_(p, d * (1.0f / len), len, t, n)) {
                telTarget_ = p + d * (t / len);
                landed = true;
                // Stand there? Level enough, not deep water, in reach, not where you are.
                const float H = player_.height;
                const float dist = length(Vec3{telTarget_.x - player_.position.x, 0, telTarget_.z - player_.position.z});
                const bool deep = telTarget_.y < player_.seaLevel - 0.6f * H;
                telValid_ = n.y > 0.75f && !deep && dist > 0.4f * s && dist < 14.0f * s;
                // Room to stand: nothing overhead within the hero's height (under a hut's eaves, inside a rock arch).
                float tu = 0;
                Vec3 nu;
                if (telValid_ && worldRay_(telTarget_ + Vec3{0, 0.1f, 0}, Vec3{0, 1, 0}, 0.95f * H, tu, nu)) telValid_ = false;
                static const bool trace = std::getenv("GVR_TELEPORT_TRACE") != nullptr;  // test hook
                if (trace && int(time_ * 4) != int((time_ - dt) * 4))
                    LOGI("teleport aim: (%.1f %.1f %.1f) %.1f m, normal y %.2f, %s%s", telTarget_.x, telTarget_.y, telTarget_.z, dist, n.y,
                         telValid_ ? "valid" : "invalid", deep ? " (deep water)" : "");
            }
            telArc_.push_back(landed ? telTarget_ : q);
            p = q;
            if (p.y < player_.position.y - 30.0f * s) break;
        }
    } else if (telAiming_ && std::fabs(st.y) < 0.25f && std::fabs(st.x) < 0.25f) {
        telAiming_ = false;
        if (telValid_ && telPhase_ == 0) {
            telPending_ = telTarget_;
            telPhase_ = 1;
            sfx2D("ph_selct", 0.4f, 1.6f);
        } else if (telPhase_ == 0) {
            sfx2D("ph_selct", 0.3f, 0.6f);  // (a low click: nowhere to stand there)
            LOGI("Teleport: no place to stand there");
        }
    } else if (!push) {
        telAiming_ = st.y > 0.25f && telAiming_;  // (easing back toward the middle keeps the aim until released)
    }
}

void GameApp::renderTeleportArc() {
    if (telArc_.size() < 2 || !text_.ready()) return;
    const Vec4 col = telValid_ ? Vec4{0.35f, 1.0f, 0.55f, 0.85f} : Vec4{1.0f, 0.35f, 0.3f, 0.7f};
    const Pose head = rig_.toWorld(lastInput_.head);
    const Quat face = head.orientation;
    // Dots along the arc (every other point), then a ring where it lands.
    for (size_t i = 1; i + 1 < telArc_.size(); i += 2) text_.drawPanel(Mat4::translation(telArc_[i]) * Mat4::rotation(face), 0.03f, 0.03f, col, true);
    const float s = std::max(1.0f, heroScale_);
    const Quat flat = Quat::fromAxisAngle({1, 0, 0}, -1.5707963f);
    const Vec3 at = telArc_.back() + Vec3{0, 0.03f, 0};
    text_.drawPanel(Mat4::translation(at) * Mat4::rotation(flat), 0.5f * s, 0.5f * s, Vec4{col.x, col.y, col.z, 0.35f}, true);
    text_.drawPanel(Mat4::translation(at + Vec3{0, 0.005f, 0}) * Mat4::rotation(flat), 0.2f * s, 0.2f * s, col, true);
}

}  // namespace gvr
