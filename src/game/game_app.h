// Platform-independent application core. The Quest runtime (OpenXR) and the headless desktop
// harness both drive this class: update() once per frame, renderEye() once per eye.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/input_state.h"
#include "game/player.h"
#include "game/weapon.h"
#include "game/enemy.h"
#include "game/creature.h"
#include "game/vr_rig.h"
#include "render/renderer.h"
#include "scene/test_scene.h"

namespace gvr {

struct EyeRenderInfo {
    Pose trackingPose;  // eye pose in tracking space
    float fovLeft, fovRight, fovUp, fovDown;
    int width, height;
};

struct HapticRequest {
    int hand;
    float amplitude;
    float seconds;
};

class AssetWorld;    // original-asset scene (defined in scene/asset_world.h)
class SkinnedModel;  // scene/skinned_model.h
struct RenderModel;  // scene/model_cache.h
class ModelCache;   // scene/model_cache.h
class AudioMixer;   // audio/mixer.h

class GameApp {
public:
    GameApp();
    ~GameApp();
    // GL context must be current. dataDir: directory holding the user's original game files
    // (the contents of the game's "Bin" folder). Missing data is not fatal.
    bool init(const std::string& dataDir, const std::string& worldName = "Story1");
    void update(float dt, const InputFrame& input);
    void renderEye(const EyeRenderInfo& eye);
    void shutdown();

    std::vector<HapticRequest>& hapticRequests() { return haptics_; }
    // Attaches the platform's audio mixer and loads the original sound effects into it.
    void setAudio(AudioMixer* mixer);

    // Play mode: you control the original Meccaryn (third person by default). Otherwise the
    // free camera (stick locomotion, Y = fly/noclip) is active. B toggles at runtime.
    bool playMode() const { return playMode_; }
    void setPlayMode(bool on, const InputFrame& in);
    bool firstPerson() const { return firstPerson_; }
    void setFirstPerson(bool on) { firstPerson_ = on; }
    PlayerController& player() { return player_; }
    void teleportPlayer(const Vec3& p, float yaw);
    size_t weaponShots() const { return weapon_.shotsFired(); }
    size_t weaponHits() const { return weapon_.hits(); }
    size_t enemiesAlive() const { return enemies_.aliveCount(); }
    size_t creaturesAlive() const { return creatures_.aliveCount(); }
    size_t characterCount() const { return chars_.size(); }
    size_t spitsLaunched() const { return spitsLaunched_; }
    size_t spitHits() const { return spitHits_; }
    size_t gibCount() const { return gibs_.size(); }
    const CreatureDirector& creatureDirector() const { return creatures_; }
    size_t enemyCount() const { return enemies_.enemies.size(); }
    float playerHealth() const { return playerHealth_; }
    // Test hook: feet position of any enemy (alive or dead).
    bool anyEnemy(Vec3& feet) const {
        if (enemies_.enemies.empty()) return false;
        feet = enemies_.enemies.front().pos;
        return true;
    }
    // Test hook: chest position of the nearest living enemy.
    bool nearestEnemy(Vec3& chest) const {
        float best = 1e30f;
        bool found = false;
        for (const auto& e : enemies_.enemies) {
            if (e.state == EnemyState::Dead) continue;
            const float d = length(e.pos - player_.position);
            if (d < best) { best = d; chest = e.pos + Vec3{0, 1.2f, 0}; found = true; }
        }
        for (const auto& c : creatures_.creatures) {
            if (c.state == CreatureState::Dead) continue;
            const float d = length(c.pos - player_.position);
            if (d < best) { best = d; chest = c.pos + Vec3{0, 0.45f * c.scale, 0}; found = true; }
        }
        return found;
    }
    float weaponLastHitDistance() const { return weapon_.lastHitDistance(); }
    VrRig& rig() { return rig_; }
    Renderer& renderer() { return renderer_; }
    bool hasOriginalAssets() const;

private:
    Renderer renderer_;
    VrRig rig_;
    TestScene testScene_;
    std::unique_ptr<AssetWorld> assetWorld_;
    InputFrame lastInput_;
    std::vector<HapticRequest> haptics_;
    ButtonEdge triggerEdge_[2];
    ButtonEdge modeEdge_;
    ButtonEdge flyEdge_;
    void updatePlayMode(float dt, const InputFrame& in);
    void placeCamera(float dt, bool snap, const InputFrame* in = nullptr);
    void poseFirstPersonBody(const InputFrame& in);
    PlayerController player_;
    std::unique_ptr<SkinnedModel> playerModel_;
    int clip_[int(PlayerAnim::Count)] = {};
    bool playMode_ = false;
    bool firstPerson_ = true;  // primary mode (user request): own body + hands
    Mat4 bodyXf_ = Mat4::identity();  // player model transform (FP: under the head, scaled)
    Vec3 eyeModel_{0, 1.9f, -0.1f};   // Meccaryn eye point, model-local metres (measured at init)
    Vec3 eyeFromHead_{0, 0, -0.2f};   // eye point relative to the head-vertex centroid
    Vec3 eyeNow_{0, 1.9f, -0.1f};     // eye point in the latest animated pose (follows lean/bob)
    float userEye_ = 1.6f;            // user's eye height estimate (smoothed tracked head height)
    bool eyeCalibrated_ = false;
    uint32_t recenterSerial_ = 0;
    Vec3 headPrev_;                   // previous tracking-space head position (physical walking)
    bool haveHeadPrev_ = false;
    struct ArmPose { bool valid = false; Vec3 grip, forward, up; float squeeze = 0, trigger = 0; };
    ArmPose armTargets_[2];           // model space (game units), per hand
    // Weapon: the original Meccaryn gun in the right hand (attached at the skeleton's Ref_Rhand).
    const RenderModel* gunModel_ = nullptr;
    const RenderModel* boltModel_ = nullptr;
    const RenderModel* muzzleModel_ = nullptr;
    const RenderModel* impactModel_ = nullptr;
    Mat4 gunXf_ = Mat4::identity();
    bool gunValid_ = false;
    WeaponSystem weapon_;
    std::function<bool(const Vec3&, const Vec3&, float, float&, Vec3&)> worldRay_;  // terrain + buildings
    // Enemies (Reaper grunts) and combat state.
    EnemyDirector enemies_;
    std::vector<std::unique_ptr<SkinnedModel>> enemyPool_;
    std::vector<uint8_t> slotUsed_;
    std::vector<float> slotLift_;   // per model slot: lift so the posed body's lowest point is on the ground
    WeaponSystem enemyFire_;
    std::vector<int> shooters_;
    float enemyFootOffset_ = 0.0f;  // rpman's origin is at the waist: lift so the feet touch the ground
    float playerHealth_ = 100.0f, sinceDamage_ = 99.0f, damageFlash_ = 0.0f, waveTimer_ = 0.0f;
    int wave_ = 0;
    TextureId whiteTex_ = 0;
    uint32_t rng_ = 987654321u;
    float frand() { rng_ = rng_ * 1664525u + 1013904223u; return float(rng_ >> 8) / float(1u << 24); }
    void spawnWave();
    // Mission characters: Rippers (CreatureDirector AI) and ambient ones (looping default clip).
    struct CharacterInst {
        std::unique_ptr<SkinnedModel> model;
        int clipIndex[int(CreatureClip::Count)] = {};
        Vec3 pos;
        float yaw = 0, scale = 1, lift = 0, time = 0, stepTimer = 0;
        bool ambient = true, visible = true;
        CreatureSize size = CreatureSize::Medium;
    };
    // Ripper spit balls (orange medium / blue large), blasts and gore from bursting Rippers.
    std::unique_ptr<SkinnedModel> spitBall_[3];
    Vec3 spitCentre_[3];
    const RenderModel* spitRing_[3] = {};
    const RenderModel* goreHead_ = nullptr;
    const RenderModel* goreArm_ = nullptr;
    struct Blast { Vec3 pos; int size; float age; };
    std::vector<Blast> blasts_;
    struct Gib { const RenderModel* model; Vec3 pos, vel, axis; float angle, spin, scale, life; };
    std::vector<Gib> gibs_;
    void spawnGore(const Vec3& pos, const Vec3& dir, float scale);
    std::vector<CharacterInst> chars_;
    CreatureDirector creatures_;
    std::vector<CreatureEvent> creatureEvents_;
    bool missionHostiles_ = false;
    size_t spitsLaunched_ = 0, spitHits_ = 0;  // the mission placed enemies: no test waves
    void initCharacters(AssetWorld* w, ModelCache* mc);
    void updateCharacters(float dt);
    void creatureSound(const CreatureEvent& e);
    void updateCombat(float dt);
    void damagePlayer(float amount);
    void renderHud();
    // Audio (original WAVs from the user's archives; names decoded by guesswork, see the log).
    AudioMixer* audio_ = nullptr;
    void sfx(const char* name, const Vec3& pos, float vol = 1.0f, float pitch = 1.0f);
    void sfx2D(const char* name, float vol = 1.0f, float pitch = 1.0f);
    void updateAudio(float dt);
    uint32_t jetLoop_ = 0, slideLoop_ = 0;
    bool wasThrusting_ = false, wasSliding_ = false, wasOnGround_ = true;
    float airTime_ = 0, prevRunPhase_ = 0, hurtCooldown_ = 0, fallSpeed_ = 0;
    std::vector<float> slotStep_;
    Quat headWorldRot_;
    // Third-person aiming: crosshair where the right controller points; character aims there.
    Vec3 aimTarget_;
    bool tpAiming_ = false;
    float tpAimHold_ = 0;
    TextureId crosshairTex_ = 0;
    void updateThirdPersonAim(const InputFrame& in, float dt);
    void updateWeapon(float dt, const InputFrame& in);
    void drawModel(const RenderModel* m, const Mat4& xf, bool additive, float alpha = 1.0f);
    float camYaw_ = 0;      // play-mode camera yaw (snap turn orbits the character)
    float camFloor_ = 0;    // smoothed tracking-floor height of the play camera
    ButtonEdge playEdge_, viewEdge_;
    bool showTestScene_ = true;
    bool needSpawn_ = false;
    double time_ = 0.0;
};

}  // namespace gvr
