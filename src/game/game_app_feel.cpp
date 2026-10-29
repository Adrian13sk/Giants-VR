// GameApp: VR feel - controller rumble and the holstered weapon.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

// Comfort vignette (rework; standard in VR games): while the stick moves you or turns you smoothly,
// the edges of the view darken so the motion your eyes see without your body feeling it shrinks to
// the middle. Off / low / high in Options.
void GameApp::updateVignette(float dt, const InputFrame& in) {
    float want = 0;
    if (settings_.vignette > 0 && playMode_ && !cutsceneActive() && !menuOpen_) {
        const Vec2 mv = in.hand[rig_.moveHand()].thumbstick;
        const float move = std::min(1.0f, std::sqrt(mv.x * mv.x + mv.y * mv.y));
        const float turn = rig_.comfort.snapTurn ? 0.0f : std::fabs(in.hand[rig_.turnHand()].thumbstick.x);
        float k = std::max(move > 0.2f ? move : 0.0f, turn > 0.2f ? turn : 0.0f);
        if (riding_ || !player_.onGround) k = std::max(k, std::min(1.0f, player_.horizontalSpeed() / 12.0f));
        // (2026-10-29) In a vehicle the view turns and climbs without the stick turning it: its turn rate (snap steps
        // aside) and the gyrocopter's climb count too.
        if (riding_ && dt > 0) {
            const float yr = std::fabs(angleDelta(vigYawPrev_, camYaw_)) / dt;
            if (yr < 0.3f / dt) k = std::max(k, std::min(1.0f, yr / 1.2f));
            if (gyro_.riding) k = std::max(k, std::min(1.0f, std::fabs(gyro_.vel.y) / 6.0f));
        }
        want = k * (settings_.vignette == 1 ? 0.55f : 0.9f);
    }
    // A cutscene camera that travels (the view moves without you): the same comfort ring, by its speed.
    if (settings_.vignette > 0 && cutsceneActive() && dt > 0) {
        const float v = length(cutEye_ - cutVigPrev_) / dt;
        cutVigPrev_ = cutEye_;
        if (v < 200.0f) want = std::max(want, std::min(1.0f, v / 15.0f) * (settings_.vignette == 1 ? 0.3f : 0.6f));  // (a cut jumps: ignored)
    } else {
        cutVigPrev_ = cutEye_;
    }
    vigYawPrev_ = camYaw_;
    vignette_ += (want - vignette_) * std::min(1.0f, dt * (want > vignette_ ? 6.0f : 3.0f));
    static const bool trace = std::getenv("GVR_VIGNETTE_TRACE") != nullptr;
    if (trace) LOGI("vignette %.2f (want %.2f, setting %d)", vignette_, want, settings_.vignette);
}

// An annulus 1 m ahead of the eyes: clear inside, a soft edge, `rgb` outside; `k` (0..1) closes it in.
void GameApp::drawEyeRing(MeshId& mesh, float k, const Vec3& rgb, float alpha) {
    const int seg = 48;
    const float inner = std::tan((60.0f - 32.0f * k) * kPi / 180.0f);
    const float soft = std::tan((75.0f - 30.0f * k) * kPi / 180.0f);
    const float outer = std::tan(85.0f * kPi / 180.0f);
    MeshData md;
    const uint8_t r8 = uint8_t(std::min(1.0f, rgb.x) * 255.0f), g8 = uint8_t(std::min(1.0f, rgb.y) * 255.0f), b8 = uint8_t(std::min(1.0f, rgb.z) * 255.0f);
    for (int i = 0; i < seg; ++i) {
        const float a = 2.0f * kPi * float(i) / float(seg);
        const float c = std::cos(a), s = std::sin(a);
        const float rs[3] = {inner, std::max(inner + 0.05f, soft), outer};
        const uint8_t al[3] = {0, 255, 255};
        for (int q = 0; q < 3; ++q) {
            Vertex v{};
            v.pos[0] = c * rs[q]; v.pos[1] = s * rs[q]; v.pos[2] = -1.0f;
            v.normal[2] = 1.0f;
            v.color[0] = r8; v.color[1] = g8; v.color[2] = b8;
            v.color[3] = al[q];
            md.vertices.push_back(v);
        }
    }
    for (int i = 0; i < seg; ++i) {
        const uint32_t a = uint32_t(i * 3), b = uint32_t(((i + 1) % seg) * 3);
        for (uint32_t q = 0; q < 2; ++q) md.indices.insert(md.indices.end(), {a + q, b + q, b + q + 1, a + q, b + q + 1, a + q + 1});
    }
    if (!mesh) mesh = renderer_.uploadMesh(md);
    else renderer_.updateMeshVertices(mesh, md.vertices);
    const Pose head = rig_.toWorld(lastInput_.head);
    DrawParams dp;
    dp.unlit = true;
    dp.blend = BlendMode::AlphaBlend;
    dp.depthTest = false;
    dp.depthWrite = false;
    dp.doubleSided = true;
    dp.noFog = true;
    dp.tint = {1, 1, 1, std::min(1.0f, alpha)};
    renderer_.draw(mesh, Mat4::translation(head.position) * Mat4::rotation(head.orientation) * Mat4::scale(Vec3{1, 1, 1} * rig_.comfort.worldScale), dp);
}

void GameApp::renderVignette() {
    if (!lastInput_.headValid) return;
    // Hurt (rework): below 40 health a dark red rim that beats like a pulse, stronger the lower.
    if (playMode_ && !dying_ && playerHealth_ > 0 && playerHealth_ < 40.0f) {
        const float low = (40.0f - playerHealth_) / 40.0f;
        const float beat = 0.65f + 0.35f * std::pow(std::max(0.0f, std::sin(float(time_) * (5.0f + 3.0f * low))), 4.0f);
        drawEyeRing(hurtRingMesh_, 0.25f + 0.45f * low, {0.45f, 0.0f, 0.0f}, (0.35f + 0.5f * low) * beat);
    }
    // A hit (rework): a red rim that flares and fades - no full-view red wash in VR.
    if (damageFlash_ > 0.02f && !settings_.classicLook && playMode_) drawEyeRing(hitRingMesh_, 0.3f + 0.35f * damageFlash_, {0.7f, 0.02f, 0.0f}, 0.75f * damageFlash_);
    if (vignette_ >= 0.02f) drawEyeRing(vignetteMesh_, vignette_, {0, 0, 0}, vignette_ * 1.1f);
}

void GameApp::updateFeel(float dt, const InputFrame& in) {
    updateVignette(dt, in);
    // (Low health: the heartbeat below - the rework had added a second one on top of it.)
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
    if (firstPerson_ && !riding_ && (hasLauncher_ || longOut)) {  // (not in a vehicle: the joystick grip isn't a draw)
        const int gh = rig_.turnHand();
        const ControllerState& c = in.hand[gh];
        const bool grip = c.squeeze > 0.7f;
        if (grip && !holsterGripHeld_ && c.gripPoseValid) {
            const Vec3 hand = rig_.toWorld(c.gripPose).position;
            const bool atGun = longOut && length(hand - holsterGunPos_) < 0.3f;
            const bool atBack = !launcherOut && hasLauncher_ && launcherModel_ && (length(hand - holsterBackPos_) < 0.42f ||
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
