// Platform-independent application core. The Quest runtime (OpenXR) and the headless desktop
// harness both drive this class: update() once per frame, renderEye() once per eye.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/input_state.h"
#include "game/player.h"
#include "game/weapon.h"
#include "game/enemy.h"
#include "game/creature.h"
#include "game/smartie.h"
#include "game/gore.h"
#include "game/destruction.h"
#include "game/particles.h"
#include "game/hud_gauge.h"
#include "render/quad_batch.h"
#include "game/vr_rig.h"
#include "render/renderer.h"
#include "render/text.h"
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
class MusicSet;     // audio/music.h
class CutscenePlayer;  // game/cutscene.h
class GameText;     // assets/gtext.h
class MusicDirector;

class GameApp {
public:
    GameApp();
    ~GameApp();
    // GL context must be current. dataDir: directory holding the user's original game files
    // (the contents of the game's "Bin" folder). Missing data is not fatal.
    bool init(const std::string& dataDir, const std::string& worldName = "Story1");
    void update(float dt, const InputFrame& input);
    void renderEye(const EyeRenderInfo& eye);
    // Once per frame, before the eyes: the sun's shadow map around the player.
    void renderShadows();
    // Same, also given this frame's two eyes (their fields of view size the reflection camera).
    void renderShadows(const EyeRenderInfo& left, const EyeRenderInfo& right);
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
    // Test hook: feet of the dead body (or blood pool) nearest the player.
    bool nearestCorpse(Vec3& feet) const {
        float best = 1e30f;
        bool found = false;
        for (const auto& e : enemies_.enemies)
            if (e.state == EnemyState::Dead && length(e.pos - player_.position) < best) { best = length(e.pos - player_.position); feet = e.pos; found = true; }
        for (const auto& c : creatures_.creatures)
            if (c.state == CreatureState::Dead && length(c.pos - player_.position) < best) { best = length(c.pos - player_.position); feet = c.pos; found = true; }
        for (const auto& d : blood_.decals)
            if (!found && d.texture == 2) { feet = d.pos; found = true; }
        return found;
    }
    size_t bloodDecals() const { return blood_.decals.size(); }
    // Test hook: a Reaper killed by a shot at `at` (blood, a body that stays for the Cleaners).
    void debugKillReaperAt(const Vec3& at, const Vec3& shotDir) {
        const int i = enemies_.spawn(at, 0.0f);
        const Vec3 p = enemies_.enemies[size_t(i)].pos + Vec3{0, 1.2f, 0};
        for (int k = 0; k < 4; ++k) weapon_.onHit(i, p, shotDir);
    }
    // Test hook: a rocket blast at game (x, y), `height` metres above the ground.
    void debugBlast(float gx, float gy, float height);
    size_t destroyedCount() const { size_t n = 0; for (const auto& d : destruct_.items) n += !d.alive(); return n; }
    void debugKillPlayer(int cause) { damagePlayer(500.0f, DeathCause(cause)); }
    // Test hook: hurt the player by `amount`, the hit coming from `fromYawDeg` (0 = ahead, 90 = right).
    void debugHurt(float amount, float fromYawDeg) {
        const float a = player_.yaw - fromYawDeg * 3.14159265f / 180.0f;
        hitFrom_ = player_.position + Vec3{-std::sin(a) * 8.0f, 1.2f, -std::cos(a) * 8.0f};
        hitDirTimer_ = 1.2f;
        damagePlayer(amount);
    }
    int syringeCount() const { return syringes_; }
    bool dying() const { return dying_; }
    void debugDumpBlood() const {
        Vec3 b;
        if (nearestCorpse(b)) LOGI("blood: body at (%.1f %.1f %.1f)", b.x, b.y, b.z);
        for (const auto& d : blood_.decals)
            LOGI("blood: decal tex %d at (%.1f %.1f %.1f) size %.2f age %.2f alpha %.2f water %d", d.texture, d.pos.x, d.pos.y, d.pos.z, d.scale(), d.age, d.alpha(), int(d.water));
        LOGI("blood: %zu drops, %zu sprays", blood_.drops.size(), blood_.sprays.size());
    }
    // Test hook: shoot the nearest body (until it bursts or falls apart).
    void debugShootCorpses(int shots) {
        for (int s = 0; s < shots; ++s)
            for (size_t i = 0; i < enemies_.enemies.size(); ++i)
                if (enemies_.enemies[i].state == EnemyState::Dead && !enemies_.enemies[i].gibbed) {
                    shootCorpse(kCorpseEnemy + int(i), enemies_.enemies[i].pos + Vec3{0, 0.3f, 0}, Vec3{0, -0.3f, -1});
                    break;
                }
    }
    size_t cleanerCount() const { return cleaners_.cleaners.size(); }
    VrRig& rig() { return rig_; }
    Renderer& renderer() { return renderer_; }
    bool hasOriginalAssets() const;
    void setShowTestScene(bool on) { showTestScene_ = on; }

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
    std::unique_ptr<SkinnedModel> playerSpare_;  // the other variant (with / without the jetpack)
    bool playerHasJet_ = true;                   // playerModel_ shows the jetpack
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
    // Rocket launcher (the RPG Timmy gives Baz at the end of 1b; mission item 3): mc_launcher_l0
    // in hand, mc_rocket projectiles with splash damage. Right stick forward switches weapons.
    const RenderModel* launcherModel_ = nullptr;
    // Ref_handle / Ref_nozzle01 of each weapon (game units, model frame; defaults = measured values).
    Vec3 gunHandle_{-0.01f, 1.23f, -0.71f}, gunMuzzle_{-0.01f, -1.84f, 0.33f};
    Vec3 launcherHandle_{0.0f, 3.85f, -0.45f}, launcherMuzzle_{0.0f, -4.58f, 0.03f};
    const RenderModel* rocketModel_ = nullptr;
    bool hasLauncher_ = false;
    int weaponSel_ = 0;  // 0 gun, 1 launcher
    bool switchArmed_ = true;
    float rocketCooldown_ = 0;
    bool rocketTriggerHeld_ = false;
    struct Rocket { Vec3 pos, vel; float life; };
    std::vector<Rocket> rockets_;
    void updateRockets(float dt);
    void explode(const Vec3& at, float radius, float damage);
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
    TextureId burnTexIds_[4] = {};  // char light/medium/heavy, embers (procedural)
    void updateBurn();
    float frand() { rng_ = rng_ * 1664525u + 1013904223u; return float(rng_ >> 8) / float(1u << 24); }
    void spawnWave();
    // Mission characters: Rippers (CreatureDirector AI) and ambient ones (looping default clip).
    struct CharacterInst {
        std::unique_ptr<SkinnedModel> model;
        int clipIndex[int(CreatureClip::Count)] = {};
        Vec3 pos;
        float yaw = 0, scale = 1, lift = 0, restLift = 0, time = 0, stepTimer = 0;
        bool ambient = true, visible = true;
        CreatureSize size = CreatureSize::Medium;
        int objective = -1;  // mission objective completed by this creature's death
        int type = -1;       // object type (hidden while a cutscene shows its own copy)
        int rescue = -1;     // index in rescue_ (1b's stranded Smarties); clipIndex = SmartieClip
        bool pooled = false; // a nest's Ripper model, lent to a hatched Ripper while it lives
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
    // Blood (original aablood.gzp textures), corpses and the Cleaners that eat them.
    BloodSystem blood_;
    CleanerDirector cleaners_;
    TextureId bloodTex_[3] = {};   // efx_splat, efx_splat2, bloodblob (pools)
    TextureId sprayTex_ = 0, dropTex_ = 0;
    MeshId goreQuad_ = 0;          // unit quad in XY, facing +Z
    const RenderModel* reaperChunk_[4] = {};  // rpman_chunk_head / body / main / hand
    const RenderModel* meccChunk_[6] = {};    // MC_chunk1..6 (the player blown apart)
    std::vector<std::unique_ptr<SkinnedModel>> cleanerPool_;
    std::vector<std::pair<uint32_t, uint32_t>> eatLoops_;  // corpse id -> eating sound voice
    std::vector<std::pair<uint32_t, float>> eatenNow_;
    std::vector<CleanerEvent> cleanerEvents_;
    void initGore(AssetWorld* w, ModelCache* mc);
    void updateGore(float dt);
    void renderGore();
    void bloodInWater(const Vec3& at, float size);
    void bleed(const Vec3& at, const Vec3& dir, bool kill, float scale);  // a hit (or a kill) on flesh
    // A body shot apart: bursts in a spray of blood or falls to pieces (Reaper chunks / Ripper gore).
    void gibCorpse(const Vec3& pos, float yaw, const Vec3& dir, bool reaper, float scale);
    bool corpseRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& target) const;
    void shootCorpse(int target, const Vec3& p, const Vec3& dir);
    // Player death: by bullets / bites he falls (mc_die), in the piranhas' water he goes under
    // (mc_drowning_die), a blast blows him apart (MC_chunk1..6). Then a fade and a respawn.
    enum class DeathCause { Shot, Bitten, Drowned, Blast, Fall };
    DeathCause lastHurt_ = DeathCause::Shot;
    Vec3 hitFrom_;              // where the last hit came from (HUD: a red mark on that side)
    float hitDirTimer_ = 0;
    bool dying_ = false, deathWasFp_ = false, playerGibbed_ = false;
    float respawnFade_ = 0;
    float deathTime_ = 0;
    DeathCause deathCause_ = DeathCause::Shot;
    int deathClip_[3] = {-1, -1, -1};  // mc_die, mc_drowning_die, mc_land_dead (same order in both player variants)
    void startDeath();
    void updateDeath(float dt, const InputFrame& in);
    std::vector<CharacterInst> chars_;
    CreatureDirector creatures_;
    RescueDirector rescue_;
    // Ripper nests: breed Rippers that charge the player; shooting shrinks a nest until it goes.
    struct NestInst {
        Vec3 pos;
        const RenderModel* model;
        Mat4 xf;
        float radius, hp, maxHp, target, shown, timer;
        bool alive;
        float dying;  // >= 0: seconds since it was destroyed
    };
    std::vector<NestInst> nests_;
    static constexpr int kNestTarget = 5000;  // weapon target ids for nests
    static constexpr int kCorpseEnemy = 6000, kCorpseCreature = 7000;  // and bodies on the ground
    static constexpr int kObjectTarget = 9000;  // destructible scenery (index into destruct_)
    // Destructible scenery: trees, rocks, houses, guard towers (game/destruction.h).
    DestructionDirector destruct_;
    std::vector<DestructEvent> destructEvents_;
    std::vector<int> objectItem_;        // world object -> destruct_ index (-1 none)
    std::vector<float> downTime_;        // per item: seconds since it came down
    struct Collapse { int item; std::unique_ptr<SkinnedModel> model; Mat4 xf; bool debris = false; };
    std::vector<Collapse> collapses_;    // guard towers playing their collapse clip
    std::vector<const RenderModel*> debrisStone_, debrisWood_, debrisTower_[2];
    int lastWorldTag_ = -1;              // world object hit by the last worldRay_ (collision tag)
    void initDestruction(AssetWorld* w, ModelCache* mc);
    void damageObject(int item, float amount, bool blast, const Vec3& from);
    void updateDestruction(float dt);
    bool treeRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& item) const;
    void throwDebris(const std::vector<const RenderModel*>& pieces, const Vec3& at, int count, float speed, float scale);
    // Effects: smoke, dust, fire, trails (original textures), drawn in batches.
    ParticleSystem fx_;
    TextureId fxTex_[int(FxTex::Count)] = {};
    QuadBatch fxBatch_[int(FxTex::Count)];
    QuadBatch bloodBatch_[5];           // splat, splat2, pool blob, drops, sprays
    bool batchesReady_ = false;
    float smokeTimer_ = 0, trailTimer_ = 0;
    void initFx(ModelCache* mc);
    void smokePuff(FxTex tex, const Vec3& at, const Vec3& vel, float size0, float size1, float life, float alpha, float rise);
    void dustBurst(const Vec3& at, float radius, int count);
    void updateFx(float dt);
    void buildBatches();
    void renderFx();
    void updateNests(float dt);
    void damageNest(int i, float amount);
    std::vector<SmartieEvent> rescueEvents_;
    int rescueLine_ = 0;
    static constexpr int kRescueNeeded = 4;
    bool autoGrip_ = false;  // GVR_AUTO_RESCUE test hook
    void updateRescue(float dt, const InputFrame& in);
    std::vector<CreatureEvent> creatureEvents_;
    bool missionHostiles_ = false;
    size_t spitsLaunched_ = 0, spitHits_ = 0;
    // Mission objectives from the mission stream's trigger objects (op 8): 1 proximity (within
    // 10 game units = 2 m of the object's bounds), 2 death. Kinds 3 (destination arrival) and 4
    // (Vimp meat) are not implemented yet. All done = mission complete.
    struct Objective { int kind; Vec3 boundsMin, boundsMax; bool done = false; };
    std::vector<Objective> objectives_;
    size_t objectivesDone_ = 0;
    bool missionComplete_ = false;
    bool advanceDone_ = false;  // the step to the next mission has started
    // Loading / briefing screen between missions: fade to black, the mission's title and objectives
    // (GText <mission>, MO_<mission>), load behind it, continue on its own.
    enum class LoadPhase { Idle, FadeOut, Show, Brief, FadeIn };
    struct LoadState {
        LoadPhase phase = LoadPhase::Idle;
        float t = 0, yaw = 0;
        int frames = 0;
        std::string mission, title, brief;
    } load_;
    void beginLoading(const std::string& mission, const InputFrame& in, bool load = true);
    // A mission's title and objectives text (GText <mission>, MO_<mission>).
    void missionText(const std::string& mission, std::string& title, std::string& brief) const;
    // Hold the move hand's thumbstick down: the current mission's briefing over the game world.
    bool briefHeld_ = false;
    void renderBriefOverlay();
    bool firstBriefDone_ = false;
    // Sun: crosses the sky slowly (never sets) and casts real-time shadows.
    float sunAzimuth_ = 2.2f;
    bool shadowsOn_ = false;
    MeshId sunRaysMesh_ = 0;
    void updateSun(float dt);
    void renderSunRays();
    // Water reflection: the world mirrored in the sea, once per frame from between the eyes.
    bool reflOn_ = false;
    float eyeFov_[2][4] = {};  // last eyes' FOV angles (left, right, up, down)
    bool haveFov_ = false;
    void renderReflection();
    void renderShadowMap();
    // Adaptive quality: if the headset can't keep its frame rate (average frame time over a few
    // seconds), effects step down: 0 full, 1 reflection every 3rd frame, 2 no reflection, 3 no shadows.
    int quality_ = 0;
    uint32_t frameNo_ = 0;
    bool reflFresh_ = false;  // the reflection texture holds a usable recent frame
    float perfTime_ = 0;
    int perfFrames_ = 0;
    void updateQuality(float dt);
    // Timmy rides on Baz's back (Ref_HolsterB, his own sm_mount clip) once Baz has the jetpack:
    // the Story 1 cutscenes after the hand-over carry him there (story1_jetpack_6/8, get_rpg9/10,
    // hatch_into_tel) and no later Story 1 mission places him anywhere else.
    std::unique_ptr<SkinnedModel> timmy_;
    Mat4 timmyXf_ = Mat4::identity();
    bool timmyOn_ = false;
    // Baz's Ref_Smartie (the rider's mount point) relative to his spine: not every clip carries
    // the reference nodes (mc_hover, mc_fly_left... have neither Ref_Smartie nor Ref_HolsterB), so
    // the mount follows the spine with this offset (game frame), measured when a clip has it.
    Mat4 smartieFromSpine_ = Mat4::identity();
    bool haveSmartieLocal_ = false;
    Mat4 timmyRestFix_ = Mat4::identity();  // (unused: spine rotation reading kept for reference)
    Vec3 timmyOffset_;
    // Timmy talks while riding: the game's Smartie chat lines (GTextEnglish smchride1-4,
    // smchpickup2, smchbaseattack1/4 - shown in the original as Smartie messages) as subtitles,
    // voiced with the Smarties' gsmNN clips at a child's pitch (that pairing is ours).
    std::string timmyLine_;
    float timmyLineT_ = 0, timmyChatCooldown_ = 4.0f;
    bool timmyGreeted_ = false;
    int recentKills_ = 0;
    void timmySay(const char* key, bool force = false);
    void renderTimmyLine();
    int timmyYell_[3] = {-1, -1, -1}, timmyClip_ = 0, timmyPrevClip_ = 0;
    float timmyClipT_ = 0, timmyPrevT_ = 0, timmyIdleT_ = 0, timmyFade_ = 1, timmyYellIn_ = 3, timmyLastTime_ = 0;                       // model-frame nudge of the seat (m)
    int timmyType_ = 583;
    void updateTimmy();
    void updateLoading(float dt, const InputFrame& in);
    void renderLoading();
    ButtonEdge missionEdge_;
    bool piranhaKnown_ = false, piranhas_ = false;
    // Water: splash / ring effects, the piranha school.
    struct WaterFx { Vec3 pos; float age; int kind; float scale; };  // kind 0 splash, 1 ring
    std::vector<WaterFx> waterFx_;
    std::unique_ptr<SkinnedModel> splashFx_, ringFx_, pirSwim_, pirJump_;
    static constexpr int kPiranhas = 5;
    struct Piranha { float angle = 0, jump = -1; };
    // A school circles its prey just under the surface: the swimming player (school 0) or a
    // Smartie that fell in (target = its rescue index). It fades in while attacking, then dives.
    struct School { Vec3 centre; float vis = 0, biteTimer = 0; bool active = false; int target = -1; Piranha fish[kPiranhas]; };
    std::vector<School> schools_;
    void startSchool(School& s);
    void updateSchools(float dt);
    bool wasSwimming_ = false, piranhaAttack_ = false;
    float ringTimer_ = 0, wadeTimer_ = 0;
    uint32_t swimLoop_ = 0;
    std::vector<float> npcRipple_;  // per creature / enemy: seconds until its next ripple in water
    float waterTime_ = 0, biteTimer_ = 0;  // after a mission (and its outro) ends: seconds until the next loads
    void loadMission(const std::string& name);
    std::string dataDir_, worldName_;
    void initMission();
    void updateMission(float dt);
    void completeObjective(int index);
    // Original adaptive music (Music/*.raw + Bin/MusicSet.bin) and cutscene soundtracks.
    std::unique_ptr<MusicSet> musicSet_;
    std::unique_ptr<MusicDirector> music_;
    std::vector<std::pair<std::string, std::string>> musicFiles_;  // lower-case segment -> path
    std::string ambientSet_ = "ma1";
    float combatHold_ = 0, streamLeft_ = 0, stingerLeft_ = 0;
    void initMusic();
    void updateMusic(float dt);
    std::string findMusicFile(const std::string& segment) const;
    // Plays the soundtrack stream of a cutscene (flk_<name>.bin names it, e.g. "f_jetpack").
    bool playFlickSoundtrack(const std::string& flick);
    // cue = a timed sound effect / voice clip inside a cutscene (plays alongside the soundtrack).
    bool playStream(const std::string& stream, bool cue = false);
    uint32_t streamVoice_ = 0;
    std::vector<uint32_t> cueVoices_;
    std::vector<std::pair<std::string, std::string>> streamFiles_;  // lower-case name -> path (indexed once)
    bool streamsIndexed_ = false;
    // Cutscenes (flk shots): fixed viewpoint per shot, fades, subtitles in the game font.
    std::unique_ptr<CutscenePlayer> cutscene_;
    std::unique_ptr<GameText> gameText_;
    TextRenderer text_;
    Vec3 cutEye_;
    float cutYaw_ = 0;
    bool skipArmed_ = false;
    Quat subRot_;          // head-locked panels: smoothed head yaw/pitch
    Vec3 subHead_;         // and head position
    bool subInit_ = false;
    void followHead(float dt, const InputFrame& in);
    void startCutscene(const std::string& first);
    void updateCutscene(float dt, const InputFrame& in);
    void renderCutscene();
public:
    size_t objectiveCount() const { return objectives_.size(); }
    bool cutsceneActive() const;
    size_t cutsceneShots() const;
    size_t objectivesDone() const { return objectivesDone_; }
    bool missionComplete() const { return missionComplete_; }
    bool jetpackAvailable() const { return player_.jetpackAvailable; }
private:  // the mission placed enemies: no test waves
    void initCharacters(AssetWorld* w, ModelCache* mc);
    void updateCharacters(float dt);
    void creatureSound(const CreatureEvent& e);
    void updateCombat(float dt);
    void damagePlayer(float amount, DeathCause cause = DeathCause::Shot);
    void renderHud();
    // HUD with the original Meccaryn HUD art (game_app_hud.cpp): the heart-shaped health gauge,
    // the jetpack's fuel gauge, health injections carried, the weapon in hand and objective pips
    // (FP: over the move-hand wrist; TP: head-locked, lower left); the weapon strip that pops up on
    // a weapon switch; a red marker toward whatever hurt the player; green flash on healing.
    enum HudLayer { kHudPlate, kHudHealthFill, kHudFuelFill, kHudEnergy, kHudJetGauge, kHudWpSlot, kHudGun, kHudLauncher,
                    kHudSyringe, kHudNumbers, kHudMarks, kHudLayers };
    struct HudFrame { Vec3 o, r, u; };  // panel origin and its right / up axes (world, metres per unit)
    TextureId hudTex_[kHudLayers] = {};
    QuadBatch hudBatch_[kHudLayers];
    std::vector<GaugeSpan> heartShape_, jetShape_;
    bool hudReady_ = false;
    float weaponHudT_ = 99.0f, pickupFlash_ = 0, healFlash_ = 0;
    void initHud(ModelCache* mc);
    void buildHud();
    void hudRect(int layer, const HudFrame& f, float x, float y, float w, float h, const uint8_t rgba[4], float u0 = 0,
                 float v0 = 0, float u1 = 1, float v1 = 1);
    void hudGauge(int fillLayer, int outlineLayer, const std::vector<GaugeSpan>& shape, const HudFrame& f, float x, float y,
                  float w, float h, float level, const uint8_t fill[4], const uint8_t outline[4]);
    void hudDigits(const HudFrame& f, float x, float y, float h, int value, const uint8_t rgba[4]);
    void hudStatus(const HudFrame& f);
    void hudWeaponStrip();
    void hudHitMarker();
    // Health injections (green syringes): picked up on the map (placed mc_pickup_health objects,
    // and dropped by some of the Reapers killed), carried (max 5), injected by hand: FP - X takes
    // one into the move hand, push it into your chest; TP - X plays the injection animation.
    struct HealthPickup { Vec3 pos; float t = 0; };
    std::vector<HealthPickup> healthPickups_;
    const RenderModel* pickupModel_ = nullptr;
    const RenderModel* syringeModel_ = nullptr;
    float pickupScale_ = 1.0f, syringeScale_ = 1.0f;
    Vec3 syringeAxis_{0, 0, -1}, syringeCentre_;  // model frame: needle direction, centre
    int syringes_ = 0;
    static constexpr int kMaxSyringes = 5;
    static constexpr float kSyringeHeal = 50.0f;
    bool syringeOut_ = false;   // FP: one in the move hand
    float syringeOutT_ = 0;
    float injectT_ = -1;        // TP: injection animation time, -1 = none
    bool injectHealed_ = false;
    float healPending_ = 0;
    ButtonEdge injectEdge_;
    Mat4 syringeXf_ = Mat4::identity();
    bool syringeValid_ = false;
    std::vector<uint8_t> enemyDead_;
    void initPickups(ModelCache* mc);
    void updatePickups(float dt, const InputFrame& in);
    void renderPickups();
    void dropHealth(const Vec3& at);
    void injectHealth();
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
