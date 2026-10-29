// (2026-10-29) Baz's gyrocopter: the data's Gyro_l0 (GText "Gyrocopter", built by the base's Gyrocopter Pad,
// s_mecc_gpad, "Gyropad ready!") - a seat on three wheels under a rotor, two guns at its sides (Ref_nozzle), a
// joystick, and its sounds (me_gyin / gyon / gylp / gyrev / gylnd / gyout / gydmg). No mission places one. Ours:
// parked by the start in practice (Baz), and on the Gyrocopter Pad once a base has built it; GVR_GYRO puts one by
// the player (test hook). How it flies is ours:
//   walk up to it and squeeze a grip (first person: a hand on it) - you're in;
//   left stick: forward / back and turn, right stick up / down: climb / descend, right trigger: its guns;
//   or take the joystick: the right grip held, the controller tilted forward / back and rolled to the sides;
//   on the ground, the left grip held 1 s: out.
// The horizon stays level (the model banks, the view doesn't).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "assets/gzp.h"
#include "assets/skin_anim.h"
#include "audio/mixer.h"
#include "core/log.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr float kGyroTop = 22.0f;     // m/s forward
constexpr float kGyroBack = 6.0f;     // m/s backward
constexpr float kGyroClimb = 6.0f;    // m/s up / down
constexpr float kGyroTurn = 1.3f;     // rad/s
constexpr float kGyroCeiling = 90.0f; // m above the ground under it
}  // namespace

void GameApp::initGyro(ModelCache* mc) {
    gyroModel_.reset();
    gyro_ = Gyro{};
    if (!mc || !assetWorld_) return;
    auto sm = std::make_unique<SkinnedModel>();
    if (!sm->load(assetWorld_->fs(), *mc, renderer_, "gyro_l0", "gyro_l0.anm")) { LOGW("Gyrocopter: gyro_l0 unavailable"); return; }
    // Its reference points (game frame) from the clip's rest pose.
    std::vector<uint8_t> b;
    AnmAnimation a;
    if (assetWorld_->fs().read("gyro_l0.anm", b) && parseAnm(b.data(), b.size(), a)) {
        std::vector<Mat4> wm;
        a.evaluate(0.0f, wm);
        auto at = [&](const char* node, Vec3& out) {
            auto it = a.byName.find(node);
            if (it != a.byName.end() && size_t(it->second) < wm.size())
                out = giantsToRuntime(wm[size_t(it->second)].m[12], wm[size_t(it->second)].m[13], wm[size_t(it->second)].m[14]);
        };
        at("ref_seat", gyroSeat_);
        at("ref_nozzle", gyroNozzle_);
        at("joystick", gyroStick_);
    }
    gyroModel_ = std::move(sm);
    // The pilot: gyro_breathe is the gyrocopter with a Meccaryn seated in it - its Meccaryn bones pose Baz there
    // (both his models: with and without the jetpack).
    gyroRideClip_ = playerModel_ ? playerModel_->addAnimation(assetWorld_->fs(), "gyro_breathe.anm") : -1;
    if (playerSpare_) playerSpare_->addAnimation(assetWorld_->fs(), "gyro_breathe.anm");
    LOGI("Gyrocopter: model ok; seat (%.2f %.2f %.2f), guns (%.2f %.2f %.2f), stick (%.2f %.2f %.2f) m", gyroSeat_.x, gyroSeat_.y, gyroSeat_.z,
         gyroNozzle_.x, gyroNozzle_.y, gyroNozzle_.z, gyroStick_.x, gyroStick_.y, gyroStick_.z);
}

void GameApp::placeGyro(const Vec3& at, float yaw, float lift) {
    if (!gyroModel_ || !assetWorld_) return;
    gyro_ = Gyro{};
    gyro_.on = true;
    gyro_.pos = at;
    float g;
    if (assetWorld_->terrain().heightAt(at.x, at.z, g)) gyro_.pos.y = g;
    gyro_.pos.y += lift;  // (standing on a pad)
    gyro_.padY = lift > 0 ? gyro_.pos.y : -1e9f;
    gyro_.padAt = gyro_.pos;
    gyro_.yaw = yaw;
    LOGI("Gyrocopter parked at (%.1f %.1f %.1f)", gyro_.pos.x, gyro_.pos.y, gyro_.pos.z);
}

// The floor under a point: the ground, the sea's surface over water (it skims, never sinks), its pad near the pad.
float GameApp::gyroFloor(const Vec3& p) const {
    float g = -1000.0f;
    if (assetWorld_ && !assetWorld_->terrain().heightAt(p.x, p.z, g)) g = -1000.0f;
    if (player_.seaLevel > -1e8f) g = std::max(g, player_.seaLevel + 0.3f);
    if (gyro_.padY > -1e8f && length(Vec3{p.x - gyro_.padAt.x, 0, p.z - gyro_.padAt.z}) < 4.0f) g = std::max(g, gyro_.padY);
    return g;
}

Mat4 GameApp::gyroXf() const {
    return Mat4::translation(gyro_.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, gyro_.yaw) * Quat::fromAxisAngle({0, 0, 1}, gyro_.bank) *
                                                         Quat::fromAxisAngle({1, 0, 0}, gyro_.tilt));
}

// Engine and beam loops off (quitting to the menu, a cutscene, the free camera).
void GameApp::stopVehicleLoops() {
    if (gyroLoop_ && audio_) { audio_->stop(gyroLoop_); gyroLoop_ = 0; }
    if (repairLoop_ && audio_) { audio_->stop(repairLoop_); repairLoop_ = 0; }
    repairOn_ = false;
}

void GameApp::gyroOut(const char* why) {
    if (!gyro_.riding) return;
    gyro_.riding = false;
    riding_ = false;
    if (gyroLoop_ && audio_) { audio_->stop(gyroLoop_); gyroLoop_ = 0; }
    sfx2D("me_gyout", 0.8f);
    // Beside it, at its floor's height (not a teleport: that would put him on the terrain under a pad or a roof, and
    // refill the jetpack); his own physics takes it from there.
    const Vec3 side{std::cos(gyro_.yaw), 0, -std::sin(gyro_.yaw)};
    player_.position = gyro_.pos + side * 2.5f + Vec3{0, 0.3f, 0};
    player_.velocity = Vec3{0, 0, 0};
    gyro_.vel = Vec3{0, 0, 0};
    gyro_.boardCool = 2.0f;
    LOGI("Baz gets out of the gyrocopter (%s)", why);
}

// Parked: walking up to it and squeezing a grip gets him in.
void GameApp::updateGyroParked(float dt, const InputFrame& in) {
    if (!gyro_.on || gyro_.riding) return;
    gyro_.boardCool = std::max(0.0f, gyro_.boardCool - dt);
    gyro_.rotor *= std::max(0.0f, 1.0f - dt * 0.6f);  // (winding down)
    gyro_.bank *= 0.9f;
    gyro_.tilt *= 0.9f;
    // Left in the air (its pilot fell): it comes down on its own.
    {
        const float fl = gyroFloor(gyro_.pos);
        if (gyro_.pos.y > fl + 0.02f) {
            gyro_.vel.y -= 9.8f * dt;
            gyro_.pos.y += gyro_.vel.y * dt;
            if (gyro_.pos.y <= fl) { gyro_.pos.y = fl; gyro_.vel.y = 0; sfx("me_gylnd", gyro_.pos, 0.9f); }
        } else {
            gyro_.pos.y = std::max(gyro_.pos.y, fl);
            gyro_.vel.y = 0;
        }
    }
    // The grips' last state every frame (one held since before - sprinting, after getting out - isn't a fresh one).
    bool fresh[2];
    for (int h = 0; h < 2; ++h) {
        const bool sq = in.hand[h].squeeze > 0.8f;
        fresh[h] = sq && !gyroGripWas_[h];
        gyroGripWas_[h] = sq;
    }
    if (hero_ != Hero::Mecc || !playMode_ || dying_ || cutsceneActive() || gyro_.boardCool > 0) return;
    const float d = length(Vec3{player_.position.x - gyro_.pos.x, 0, player_.position.z - gyro_.pos.z});
    if (d > 3.5f || std::fabs(player_.position.y - gyro_.pos.y) > 2.5f) return;
    if (!gyroHinted_) {
        gyroHinted_ = true;
        timmyLine_ = "THE GYROCOPTER: SQUEEZE A GRIP TO GET IN";
        timmyLineT_ = 3.0f;
    }
    static const bool autoIn = std::getenv("GVR_GYRO_AUTO") != nullptr;  // test hook: in at once
    bool in2 = autoIn;
    const Vec4 seatW = gyroXf() * Vec4{gyroSeat_.x, gyroSeat_.y, gyroSeat_.z, 1.0f};
    for (int h = 0; h < 2 && !in2; ++h) {
        if (!fresh[h] || !in.hand[h].gripPoseValid) continue;
        // First person: a hand on it (within 1.8 m of the seat); third person: the gun hand's grip (the other's is sprint).
        if (firstPerson_) in2 = length(rig_.toWorld(in.hand[h].gripPose).position - Vec3{seatW.x, seatW.y, seatW.z}) < 1.8f;
        else in2 = h == rig_.turnHand();
    }
    if (!in2) return;
    // In.
    gyro_.riding = true;
    riding_ = true;
    gyro_.vel = Vec3{0, 0, 0};
    gyro_.stickHeld = false;
    // The view: the gyrocopter's forward where the user is looking now (not where the tracking space faces).
    gyro_.headYaw0 = in.headValid ? yawOf(in.head.orientation) : 0.0f;
    gyro_.head0 = in.headValid ? in.head.position : Vec3{0, 1.6f, 0};
    camYaw_ = gyro_.yaw - gyro_.headYaw0;
    gyro_.viewYaw = camYaw_;
    player_.yaw = gyro_.yaw;
    player_.velocity = Vec3{};
    if (mgLoop_ && audio_) { audio_->stop(mgLoop_); mgLoop_ = 0; }
    timmyOn_ = false;  // (Timmy, riding on his back, gets off for the flight)
    sfx2D("me_gyin", 0.9f);
    sfx2D("me_gyon", 0.8f);
    if (audio_ && !gyroLoop_) gyroLoop_ = audio_->play2D("me_gylp", 0.45f, 0.8f, true);
    for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.5f, 0.15f});
    timmyLine_ = "LEFT STICK: FLY AND TURN - RIGHT STICK: UP / DOWN - TRIGGER: GUNS - LANDED, HOLD THE LEFT GRIP: OUT";
    timmyLineT_ = 5.0f;
    LOGI("Baz takes the gyrocopter");
}

void GameApp::updateGyroRide(float dt, const InputFrame& in) {
    const int mh = rig_.moveHand(), th = rig_.turnHand();
    float fwdIn = in.hand[mh].thumbstick.y, turnIn = in.hand[mh].thumbstick.x, climbIn = in.hand[th].thumbstick.y;
    // The joystick: the turn hand's grip held - tilting the controller forward / back and rolling it fly.
    const ControllerState& tc = in.hand[th];
    const bool stick = firstPerson_ && tc.gripPoseValid && tc.squeeze > 0.7f;
    if (stick) {
        const Vec3 f = tc.gripPose.orientation.rotate(Vec3{0, 0, -1}), r = tc.gripPose.orientation.rotate(Vec3{1, 0, 0});
        const float pitch = std::asin(std::max(-1.0f, std::min(1.0f, f.y))), roll = std::asin(std::max(-1.0f, std::min(1.0f, r.y)));
        if (!gyro_.stickHeld) {
            gyro_.stickHeld = true;
            gyro_.stickPitch0 = pitch;
            gyro_.stickRoll0 = roll;
            haptics_.push_back({th, 0.4f, 0.06f});
            LOGI("Gyrocopter: the joystick taken");
        }
        const float sp = std::max(-1.0f, std::min(1.0f, -(pitch - gyro_.stickPitch0) / 0.45f));  // (tilted forward: nose down, faster)
        const float sr = std::max(-1.0f, std::min(1.0f, -(roll - gyro_.stickRoll0) / 0.45f));
        if (std::fabs(sp) > std::fabs(fwdIn)) fwdIn = sp;
        if (std::fabs(sr) > std::fabs(turnIn)) turnIn = sr;
    } else {
        gyro_.stickHeld = false;
    }
    static const char* autoFly = std::getenv("GVR_GYRO_FLY");  // test hook: "fwd climb turn" held throughout
    if (autoFly) std::sscanf(autoFly, "%f %f %f", &fwdIn, &climbIn, &turnIn);
    if (std::fabs(fwdIn) < 0.12f) fwdIn = 0;
    if (std::fabs(turnIn) < 0.12f) turnIn = 0;
    if (std::fabs(climbIn) < 0.2f) climbIn = 0;
    // The floor under it: the ground, or the sea's surface over water (it skims, it doesn't sink).
    auto floorAt = [&](const Vec3& p) { return gyroFloor(p); };
    float ground = floorAt(gyro_.pos);
    const bool landed = gyro_.pos.y <= ground + 0.05f;
    // Turning (on the ground only while it rolls).
    const Vec3 f0{-std::sin(gyro_.yaw), 0, -std::cos(gyro_.yaw)};
    const float fwdSpeed = dot(gyro_.vel, f0);
    if (!landed || std::fabs(fwdSpeed) > 0.5f) gyro_.yaw -= turnIn * kGyroTurn * dt;
    // The view turns with it - in steps when the user plays with snap turning (smoothly otherwise).
    {
        const float want = gyro_.yaw - gyro_.headYaw0;
        if (settings_.turn <= 1) {
            const float step = settings_.turn == 0 ? 0.5236f : 0.7854f;
            const float diff = angleDelta(gyro_.viewYaw, want);
            if (std::fabs(diff) >= step) gyro_.viewYaw += diff > 0 ? step : -step;
        } else {
            gyro_.viewYaw = want;
        }
        camYaw_ = gyro_.viewYaw;
    }
    player_.yaw = gyro_.yaw;
    const Vec3 fwd{-std::sin(gyro_.yaw), 0, -std::cos(gyro_.yaw)};
    // Speed along its heading eases toward the stick's; the sideways slip dies away; climb as asked.
    const float want = fwdIn >= 0 ? fwdIn * kGyroTop : fwdIn * kGyroBack;
    float along = dot(gyro_.vel, fwd);
    along += std::max(-9.0f * dt, std::min(9.0f * dt, want - along));
    Vec3 side = gyro_.vel - fwd * dot(gyro_.vel, fwd);
    side.y = 0;
    side = side * std::max(0.0f, 1.0f - 3.0f * dt);
    float vy = gyro_.vel.y + std::max(-8.0f * dt, std::min(8.0f * dt, climbIn * kGyroClimb - gyro_.vel.y));
    if (landed && climbIn <= 0) vy = std::max(0.0f, vy);
    gyro_.vel = fwd * along + side + Vec3{0, vy, 0};
    // Something solid ahead: stopped short.
    const float hs = length(Vec3{gyro_.vel.x, 0, gyro_.vel.z});
    if (worldRay_ && hs > 0.5f) {
        float t;
        Vec3 n;
        const Vec3 dir = Vec3{gyro_.vel.x, 0, gyro_.vel.z} * (1.0f / hs);
        if (worldRay_(gyro_.pos + Vec3{0, 1.2f, 0}, dir, hs * dt + 2.5f, t, n) && t < 2.5f) {
            if (hs > 10.0f) { sfx2D("me_gydmg", 0.8f); damagePlayer(4.0f, DeathCause::Fall); }  // (a crash)
            gyro_.vel.x = gyro_.vel.z = 0;
            if (!gyro_.riding) return;  // (the crash killed him: out already)
        }
    }
    gyro_.pos = gyro_.pos + gyro_.vel * dt;
    ground = floorAt(gyro_.pos);
    if (gyro_.pos.y < ground) {
        if (gyro_.vel.y < -5.0f) { sfx2D("me_gylnd", 0.9f); haptics_.push_back({kLeft, 0.6f, 0.12f}); haptics_.push_back({kRight, 0.6f, 0.12f}); }
        gyro_.pos.y = ground;
        gyro_.vel.y = 0;
    }
    if (gyro_.pos.y > ground + kGyroCeiling) { gyro_.pos.y = ground + kGyroCeiling; gyro_.vel.y = std::min(0.0f, gyro_.vel.y); }
    // Looks: banked into a turn, nose down with speed; the rotor spins up.
    gyro_.bank += (turnIn * 0.35f * std::min(1.0f, std::fabs(along) / 8.0f + 0.3f) - gyro_.bank) * std::min(1.0f, 3.0f * dt);
    gyro_.tilt += (-along / kGyroTop * 0.18f - gyro_.tilt) * std::min(1.0f, 3.0f * dt);
    gyro_.rotor = std::min(1.0f, gyro_.rotor + dt * 0.8f);
    if (gyroLoop_ && audio_) audio_->setPitch(gyroLoop_, 0.8f + 0.5f * std::min(1.0f, length(gyro_.vel) / kGyroTop) + 0.15f * std::fabs(climbIn));
    // Baz rides it (his feet on its floor: the tracked head gives his seated height); the view goes with it.
    // First person the eyes go where the seated pilot's are (the seat, 0.55 m up), whatever the user's own height:
    // the tracking floor is set under them by the tracked head's height.
    player_.position = gyro_.pos;
    if (firstPerson_ && in.headValid) {
        // (review: from its position and heading only - not its bank and pitch, which slid the eye about - and the
        // head's movement since boarding still moves the eye: leaning and ducking work)
        const float ws = rig_.comfort.worldScale;
        const Quat yq = Quat::fromAxisAngle({0, 1, 0}, gyro_.yaw);
        const Vec3 e = gyro_.pos + yq.rotate(Vec3{gyroSeat_.x, gyroSeat_.y + 0.55f, gyroSeat_.z + 0.05f});
        const Vec3 dh = Quat::fromAxisAngle({0, 1, 0}, camYaw_).rotate(Vec3{in.head.position.x - gyro_.head0.x, 0, in.head.position.z - gyro_.head0.z}) * ws;
        player_.position = Vec3{e.x + dh.x, e.y - gyro_.head0.y * ws - seatLift() * ws, e.z + dh.z};
    }
    player_.velocity = gyro_.vel;
    player_.onGround = true;
    placeCamera(dt, false, &in);
    bodyXf_ = gyroXf();
    armTargets_[0].valid = armTargets_[1].valid = false;  // (the clip's arms: no stale targets from before boarding)
    if (playerModel_ && gyroRideClip_ >= 0) {
        playerModel_->setHideEnabled(firstPerson_);  // (first person: his head hidden round the eyes)
        const float len = std::max(0.1f, playerModel_->animDuration(gyroRideClip_));
        playerModel_->poseBlend(renderer_, gyroRideClip_, std::fmod(float(time_), len), gyroRideClip_, std::fmod(float(time_), len), 0.0f);
    }
    // Its guns: the turn hand's trigger, along its heading and the head's pitch.
    const bool trig = in.hand[th].trigger > 0.6f;
    {
        Vec3 aim = fwd;
        if (in.headValid) {
            const Vec3 g = rig_.toWorld(in.head).orientation.rotate(Vec3{0, 0, -1});
            const float p = std::max(-0.5f, std::min(0.3f, std::asin(std::max(-1.0f, std::min(1.0f, g.y)))));
            aim = normalize(fwd * std::cos(p) + Vec3{0, std::sin(p), 0});
        }
        const Vec4 nz = gyroXf() * Vec4{gyroNozzle_.x, gyroNozzle_.y, gyroNozzle_.z, 1.0f};
        const Vec3 muzzle{nz.x, nz.y, nz.z};
        if (weapon_.update(dt, trig, muzzle + aim * 0.5f, aim)) {
            haptics_.push_back({th, 0.3f, 0.03f});
            addFlash(muzzle, {1.6f, 0.9f, 0.5f}, 4.0f, 0.06f);
            sfx((weapon_.shotsFired() & 1) ? "me_peas1" : "me_peas2", muzzle, 0.7f, 0.85f + 0.1f * frand());
        }
    }
    // Out: landed, the move hand's grip held a second.
    if (landed && in.hand[mh].squeeze > 0.8f) {
        gyro_.outT += dt;
        float tg = -1000.0f;
        const bool overSea = assetWorld_ && (!assetWorld_->terrain().heightAt(gyro_.pos.x, gyro_.pos.z, tg) || tg < player_.seaLevel - 0.3f) &&
                             !(gyro_.padY > -1e8f && length(Vec3{gyro_.pos.x - gyro_.padAt.x, 0, gyro_.pos.z - gyro_.padAt.z}) < 4.0f);
        if (gyro_.outT > 1.0f && overSea) {
            gyro_.outT = -1.0f;
            timmyLine_ = "NOT OVER THE WATER - LAND ON THE GROUND FIRST";
            timmyLineT_ = 2.0f;
        } else if (gyro_.outT > 1.0f) { gyro_.outT = 0; gyroOut("the grip held"); }
    } else {
        gyro_.outT = 0;
    }
    static const bool trace = std::getenv("GVR_GYRO_TRACE") != nullptr;  // test hook
    if (trace && int(time_ * 2) != int((time_ - dt) * 2))
        LOGI("gyro trace: (%.1f %.1f %.1f) %.1f m up, speed %.1f, yaw %.0f deg%s [player %.1f %.1f %.1f]", gyro_.pos.x, gyro_.pos.y, gyro_.pos.z, gyro_.pos.y - ground,
             length(gyro_.vel), gyro_.yaw * 57.3f, stick ? ", on the joystick" : "", player_.position.x, player_.position.y, player_.position.z);
}

void GameApp::renderGyro() {
    if (!gyroModel_ || !gyro_.on) return;
    // The prop: its clip (0.3 s a turn) run at the rotor's speed.
    gyro_.rotorT += frameDt_ * (0.15f + 0.85f * gyro_.rotor) * 3.0f;
    const float d = std::max(0.05f, gyroModel_->animDuration());
    gyroModel_->pose(renderer_, std::fmod(gyro_.rotorT, d));
    gyroModel_->render(renderer_, gyroXf());
}

}  // namespace gvr
