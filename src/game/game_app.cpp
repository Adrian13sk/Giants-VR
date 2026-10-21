#include "game/game_app.h"

#include "core/log.h"
#include "scene/asset_world.h"
#include "game/arm_ik.h"
#include "scene/skinned_model.h"

#include <cmath>

namespace gvr {

GameApp::GameApp() = default;
GameApp::~GameApp() = default;

bool GameApp::init(const std::string& dataDir, const std::string& worldName) {
    if (!renderer_.init()) {
        LOGE("Renderer init failed");
        return false;
    }
    if (!testScene_.init(renderer_)) {
        LOGE("Test scene init failed");
        return false;
    }
    assetWorld_ = std::make_unique<AssetWorld>();
    if (!assetWorld_->init(dataDir, renderer_, worldName)) {
        LOGW("Original assets unavailable: %s", assetWorld_->status().c_str());
    }
    if (assetWorld_->hasTerrain()) {
        showTestScene_ = false;
        needSpawn_ = true;
        AssetWorld* w = assetWorld_.get();
        // Walk on the original terrain; keep the current height when off the heightfield.
        rig_.groundHeight = [w, this](float x, float z) {
            float y;
            return w->terrain().heightAt(x, z, y) ? y : rig_.worldFromTracking.position.y;
        };
    } else {
        rig_.groundHeight = [](float, float) { return 0.0f; };
    }
    // Player character: the original Meccaryn with its original clips.
    if (assetWorld_->hasTerrain()) {
        auto m = std::make_unique<SkinnedModel>();
        if (m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "mc_l0", "mc_stand.anm")) {
            const char* clips[] = {"mc_stand.anm", "mc_run.anm", "mc_thrust.anm", "mc_hover.anm", "mc_land_soft.anm", "mc_sidestep.anm"};
            for (int i = 1; i < int(PlayerAnim::Count); ++i) clip_[i] = std::max(0, m->addAnimation(assetWorld_->fs(), clips[i]));
            // Eye point from the idle pose: at the front of the face (-Z), so looking down shows
            // the chest and arms instead of the inside of the collar armour.
            m->pose(renderer_, 0.0f);
            Vec3 hmn, hmx;
            if (m->boneVertexBounds("head", hmn, hmx))
                eyeModel_ = {0.5f * (hmn.x + hmx.x), hmn.y + 0.55f * (hmx.y - hmn.y), hmn.z - 0.04f};
            LOGI("Player eye point (model, m): %.2f %.2f %.2f", eyeModel_.x, eyeModel_.y, eyeModel_.z);
            m->setHiddenBones({"head", "neck"});
            m->pose(renderer_, 0.0f);
            eyeFromHead_ = eyeModel_ - m->hiddenCentroid();
            eyeNow_ = eyeModel_;
            // First-person arm IK: the Meccaryn's arms reach for the tracked controllers.
            m->setPoseModifier([this](const AnmAnimation& clip, const std::vector<std::vector<int>>& children,
                                      std::vector<Mat4>& world) {
                if (!playMode_ || !firstPerson_) return;
                auto node = [&](const char* n) {
                    auto it = clip.byName.find(n);
                    return it == clip.byName.end() ? -1 : it->second;
                };
                static const char* names[2][12] = {
                    {"arm l", "wrist l", "hand l", "finger 1l1", "finger 2l1", "finger 3l1", "finger 4l1",
                     "finger 1l2", "finger 2l2", "finger 3l2", "finger 4l2", "shoulder l"},
                    {"arm r", "wrist r", "hand r", "finger 1r1", "finger 2r1", "finger 3r1", "finger 4r1",
                     "finger 1r2", "finger 2r2", "finger 3r2", "finger 4r2", "shoulder r"}};
                for (int h = 0; h < 2; ++h) {
                    if (!armTargets_[h].valid) continue;
                    ArmChain c;
                    c.upper = node(names[h][0]);
                    c.fore = node(names[h][1]);
                    c.hand = node(names[h][2]);
                    c.clavicle = node(names[h][11]);
                    for (int f = 0; f < 4; ++f) {
                        c.finger[f] = node(names[h][3 + f]);
                        c.fingerMid[f] = node(names[h][7 + f]);
                    }
                    ArmTarget t;
                    t.grip = armTargets_[h].grip;
                    t.forward = armTargets_[h].forward;
                    t.up = armTargets_[h].up;
                    // Elbows hang down, out and back (game model space: +X = left, +Y = back, +Z = up)
                    // and swing opposite the controller's up vector (rolling the hand moves the elbow).
                    const Vec3 up = normalize(t.up);
                    t.pole = Vec3{h == kLeft ? 0.7f : -0.7f, 0.4f, -1.0f} - up * 0.5f;
                    t.handScale = 0.8f;   // the gloves read oversized at first-person distance
                    t.armStretch = 1.3f;  // Meccaryn arms are short for human reach (0.49 vs ~0.6 m)
                    solveArmIk(world, children, c, t);
                    // Grip closes the hand; the trigger curls the leading finger (Meccaryn: 4 digits).
                    const float g = armTargets_[h].squeeze, tr = armTargets_[h].trigger;
                    const float curl[4] = {std::max(g, tr), std::max(g * 0.9f, 0.25f * tr), g, g};
                    curlFingers(world, children, c, curl);
                }
            });
            playerModel_ = std::move(m);
            playMode_ = true;
            AssetWorld* w = assetWorld_.get();
            player_.groundHeight = [w, this](float x, float z) {
                float y;
                return w->terrain().heightAt(x, z, y) ? y : player_.position.y;
            };
        } else {
            LOGW("Player model unavailable; free camera only");
        }
    }
    return true;
}

void GameApp::teleportPlayer(const Vec3& p, float yaw) {
    player_.teleport(p, yaw);
    camYaw_ = yaw;
    placeCamera(0.0f, true);
}

void GameApp::setPlayMode(bool on, const InputFrame& in) {
    if (on == playMode_ || (on && !playerModel_)) return;
    playMode_ = on;
    if (on) {
        // Re-enter play mode where the free camera is: the character drops in below the head.
        const Pose head = in.headValid ? rig_.toWorld(in.head) : rig_.worldFromTracking;
        camYaw_ = yawOf(rig_.worldFromTracking.orientation);
        player_.teleport(head.position, yawOf(head.orientation));
        placeCamera(0.0f, true, &in);
    }
    LOGI("Play mode %s", on ? "ON (Meccaryn)" : "OFF (free camera)");
}

// Third person: the tracking floor sits behind and above the character (rigid horizontal
// follow, no lag: lag while stick-moving causes discomfort); vertical motion is smoothed so
// terrain bumps and jetpack flights do not bob the view. First person: the floor is at the
// character's feet. Snap turn changes camYaw_, which orbits the camera around the character.
void GameApp::placeCamera(float dt, bool snap, const InputFrame* in) {
    constexpr float kBack = 3.6f, kUp = 1.1f, kSmooth = 0.12f;
    const Quat q = Quat::fromAxisAngle({0, 1, 0}, camYaw_);
    Vec3 pos = player_.position;
    if (firstPerson_) {
        // The player position IS the head (horizontally): the tracking origin is placed so the
        // tracked head lands on it; the floor is at the character's feet.
        if (in && in->headValid) pos = pos - q.rotate(Vec3{in->head.position.x, 0, in->head.position.z});
        rig_.worldFromTracking = Pose{q, pos};
        camFloor_ = player_.position.y;
        return;
    }
    float floorY = player_.position.y + (firstPerson_ ? 0.0f : kUp);
    if (!firstPerson_) {
        pos += q.rotate(Vec3{0, 0, kBack});
        if (player_.groundHeight) floorY = std::max(floorY, player_.groundHeight(pos.x, pos.z) + 0.3f);
    }
    if (snap || dt <= 0) camFloor_ = floorY;
    else camFloor_ += (floorY - camFloor_) * (1.0f - std::exp(-dt / kSmooth));
    pos.y = firstPerson_ ? player_.position.y : camFloor_;
    rig_.worldFromTracking = Pose{q, pos};
}

void GameApp::updatePlayMode(float dt, const InputFrame& in) {
    const int mh = rig_.moveHand(), th = rig_.turnHand();
    if (viewEdge_.pressed(in.hand[th].thumbstickClick)) {
        firstPerson_ = !firstPerson_;
        LOGI("Camera: %s person", firstPerson_ ? "first" : "third");
    }
    const Quat qOld = Quat::fromAxisAngle({0, 1, 0}, camYaw_);
    camYaw_ += rig_.turnInput(dt, in);
    player_.faceView = firstPerson_;
    if (in.recenterSerial != recenterSerial_) {
        // Recentre moves the tracking origin: do not turn that jump into walking.
        recenterSerial_ = in.recenterSerial;
        haveHeadPrev_ = false;
        LOGI("Recentre: head %.2f %.2f %.2f, eye estimate %.2f", in.head.position.x, in.head.position.y,
             in.head.position.z, userEye_);
    }
    if (in.headValid) {
        // Physical walking moves the player (first person).
        if (firstPerson_ && haveHeadPrev_)
            player_.position += qOld.rotate(Vec3{in.head.position.x - headPrev_.x, 0, in.head.position.z - headPrev_.z});
        headPrev_ = in.head.position;
        haveHeadPrev_ = true;
        // Eye height estimate for the body scale: smoothed, ignoring implausible values, so the
        // brief lift while putting the headset on does not inflate the body (old running max did).
        const float hy = in.head.position.y;
        if (hy > 1.0f && hy < 2.05f) {
            if (!eyeCalibrated_) { userEye_ = hy; eyeCalibrated_ = true; }
            else if (dt > 0) userEye_ += (hy - userEye_) * (1.0f - std::exp(-dt / 2.5f));
        }
    }
    PlayerInput pi;
    pi.move = in.hand[mh].thumbstick;
    pi.viewYaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : camYaw_;
    pi.jet = in.hand[th].primary;              // A
    pi.sprint = in.hand[mh].squeeze > 0.5f;    // move-hand grip
    player_.update(dt, pi);
    placeCamera(dt, false, &in);
    if (!playerModel_) return;
    if (firstPerson_) {
        poseFirstPersonBody(in);
    } else {
        bodyXf_ = Mat4::translation(player_.position) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, player_.yaw));
        armTargets_[0].valid = armTargets_[1].valid = false;
    }
    playerModel_->setHideEnabled(firstPerson_);
    playerModel_->poseBlend(renderer_, clip_[int(player_.prevAnim())], player_.prevAnimTime(),
                            clip_[int(player_.anim())], player_.animTime(), player_.fadeWeight());
    eyeNow_ = playerModel_->hiddenCentroid() + eyeFromHead_;
}

// First-person body: scaled so its eyes are at the user's real eye height, placed so its eye
// point sits under the tracked head, feet on the character's ground; arm IK targets from the
// controllers (converted to the model's game-unit frame).
void GameApp::poseFirstPersonBody(const InputFrame& in) {
    const float scale = eyeModel_.y > 0.1f ? userEye_ / eyeModel_.y : 1.0f;
    const Quat bodyRot = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
    const Vec3 head = in.headValid ? rig_.toWorld(in.head).position : player_.position;
    // Pin the eye point of the CURRENT animated pose (previous frame) under the head, so the
    // body moves back as the run clip leans the torso forward instead of the shoulders
    // swinging into view. Scale uses the upright idle eye height.
    Vec3 base = head - bodyRot.rotate(Vec3{eyeNow_.x, 0, eyeNow_.z} * scale);
    // Vertical pin: the body's eyes are always at the viewer's eyes, so the torso keeps its
    // place under the view (crouching sinks the legs instead of the chest rising into view).
    base.y = std::max(player_.position.y - 1.2f, std::min(player_.position.y + 0.5f, head.y - eyeNow_.y * scale));
    bodyXf_ = Mat4::translation(base) * Mat4::rotation(bodyRot) * Mat4::scale({scale, scale, scale});
    const Quat inv = bodyRot.conjugate();
    const float u = gGameUnitsToMeters;
    auto toModelPoint = [&](const Vec3& w) {
        const Vec3 l = inv.rotate(w - base) * (1.0f / scale);
        return Vec3{-l.x / u, l.z / u, l.y / u};
    };
    auto toModelDir = [&](const Vec3& w) {
        const Vec3 l = inv.rotate(w);
        return Vec3{-l.x, l.z, l.y};
    };
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        ArmPose& a = armTargets_[h];
        a.valid = c.gripPoseValid;
        if (!a.valid) continue;
        const Pose grip = rig_.toWorld(c.gripPose);
        const Pose aim = c.aimPoseValid ? rig_.toWorld(c.aimPose) : grip;
        a.grip = toModelPoint(grip.position);
        a.forward = toModelDir(aim.orientation.rotate(Vec3{0, 0, -1}));
        a.up = toModelDir(aim.orientation.rotate(Vec3{0, 1, 0}));
        a.squeeze = c.squeeze;
        a.trigger = c.trigger;
    }
}

bool GameApp::hasOriginalAssets() const { return assetWorld_ && assetWorld_->loaded(); }

void GameApp::update(float dt, const InputFrame& in) {
    time_ += dt;
    if (needSpawn_ && in.headValid) {
        rig_.teleportHead(in, assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
        if (playMode_) teleportPlayer(assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
        needSpawn_ = false;
    }
    // B (turn hand's secondary) toggles play mode / free camera.
    if (playEdge_.pressed(in.hand[rig_.turnHand()].secondary)) {
        setPlayMode(!playMode_, in);
        haptics_.push_back({rig_.turnHand(), 0.6f, 0.08f});
    }
    if (playMode_) {
        updatePlayMode(dt, in);
    } else {
        // Y (B when left-handed) toggles the debug fly/noclip mode. Strong buzz = on.
        const int mh = rig_.moveHand();
        if (flyEdge_.pressed(in.hand[mh].secondary)) {
            rig_.flyMode = !rig_.flyMode;
            LOGI("Fly/noclip mode %s", rig_.flyMode ? "ON" : "OFF");
            haptics_.push_back({mh, rig_.flyMode ? 0.9f : 0.3f, rig_.flyMode ? 0.15f : 0.05f});
        }
        rig_.update(dt, in);
    }
    if (assetWorld_) assetWorld_->update(dt);

    // Haptic confirmation on trigger press (validates the haptics path on device).
    for (int h = 0; h < 2; ++h)
        if (triggerEdge_[h].pressed(in.hand[h].trigger > 0.6f))
            haptics_.push_back({h, 0.5f, 0.05f});
    // Left thumbstick click toggles the engineering test geometry.
    if (modeEdge_.pressed(in.hand[kLeft].thumbstickClick)) showTestScene_ = !showTestScene_;
    lastInput_ = in;
}

void GameApp::renderEye(const EyeRenderInfo& eye) {
    Pose eyeWorld = rig_.toWorld(eye.trackingPose);
    ViewParams v;
    v.view = Mat4::fromPose(eyeWorld.inverse());
    v.projection = Mat4::projectionFov(eye.fovLeft, eye.fovRight, eye.fovUp, eye.fovDown, 0.05f, 0.0f);
    v.eyePosition = eyeWorld.position;
    renderer_.beginView(v, eye.width, eye.height, true);
    if (assetWorld_ && assetWorld_->loaded()) assetWorld_->render(renderer_);
    if (playMode_ && playerModel_) playerModel_->render(renderer_, bodyXf_);
    if (showTestScene_ || !hasOriginalAssets()) testScene_.render(renderer_, rig_, lastInput_);
    else if (!(playMode_ && firstPerson_ && playerModel_)) testScene_.renderControllers(renderer_, rig_, lastInput_);
    renderer_.endView();
}

void GameApp::shutdown() {
    playerModel_.reset();
    assetWorld_.reset();
    renderer_.shutdown();
}

}  // namespace gvr
