// Platform-independent application core. The Quest runtime (OpenXR) and the headless desktop
// harness both drive this class: update() once per frame, renderEye() once per eye.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "game/input_state.h"
#include "game/player.h"
#include "game/skirmish.h"
#include "game/sk_net.h"
#include "game/weapon.h"
#include "assets/weapondefs.h"
#include "assets/fx_defs.h"
#include "game/fx_player.h"
#include "game/enemy.h"
#include "game/creature.h"
#include "game/smartie.h"
#include "game/gore.h"
#include "game/destruction.h"
#include "game/particles.h"
#include "game/hud_gauge.h"
#include "game/villager.h"
#include "game/vimp.h"
#include "game/jetski.h"
#include "game/save_game.h"
#include "game/settings.h"
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

Mat4 affineInverse(const Mat4& m);  // rotation/scale/mirror + translation

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
    // Where progress is saved (a checkpoint at each mission start); set before init() to continue
    // from the saved mission. Empty = no saving.
    void setSavePath(const std::string& path) { savePath_ = path; }
    // Start at the main menu (title screen on the intro island) instead of straight in the game.
    void setMainMenu(bool on) { titleWanted_ = on; }
    bool inMainMenu() const { return title_; }
    void debugCompleteMission() { missionComplete_ = true; }  // test hook (harness --complete-at)
    // Test hook: reload the current mission through the loading screen and briefing.
    void debugReloadWithBriefing(const InputFrame& in);
    int effectsQuality() const { return quality_; }  // adaptive fallback level (0 = full effects)
    void debugSetVimpMeat(int n) { vimpMeat_ = n; }  // test hook (harness --vimp-meat)
    void update(float dt, const InputFrame& input);
    void renderEye(const EyeRenderInfo& eye);
    // Both eyes in one pass into a two-layer framebuffer (renderer().multiviewSupported()).
    void renderStereo(const EyeRenderInfo& left, const EyeRenderInfo& right);
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
    // Fixed foveated rendering level the player chose (the platform layer applies it; -1 = leave alone).
    // The Options setting, one step stronger while the adaptive fallback is on (quality 1+: the
    // scenes that miss 72 Hz on the headset are GPU-bound); test runs (GVR_FFR) pin it.
    int foveationWanted() const {
        return foveationOverride_ >= 0 ? foveationOverride_ : std::min(3, settings_.foveation + (quality_ >= 1 ? 1 : 0));
    }
    void setFoveationOverride(int level) { foveationOverride_ = level; }
    bool playMode() const { return playMode_; }
    void setPlayMode(bool on, const InputFrame& in);
    bool firstPerson() const { return firstPerson_; }
    void setFirstPerson(bool on) { firstPerson_ = on; }
    PlayerController& player() { return player_; }
    const AssetWorld* assetWorld() const { return assetWorld_.get(); }  // (profiling)
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
        for (const auto& c : chars_) {  // mission bosses: the middle of the body (Kabuto's weak spot)
            if (!c.boss || c.hp <= 0) continue;
            const float d = length(c.pos - player_.position);
            if (d < best) { best = d; chest = c.pos + Vec3{0, 0.48f * c.bossH, 0}; found = true; }
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
    // Test hook: a live Reaper standing at `at` (index, -1 if none).
    int debugSpawnReaperAt(const Vec3& at) { return enemies_.spawn(at, 0.0f); }
    // Test hook: a rocket blast at game (x, y), `height` metres above the ground.
    void debugBlast(float gx, float gy, float height);
    // Test scripts ("reapers"): n Reapers in a row (1 m apart) dist m in front of the player, facing him; then a
    // rocket blast on each of them.
    void debugReapersAhead(int n, float dist);
    void debugBlastReapers();
    std::vector<int> debugReapers_;
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
    Vec3 meccEye_, meccEyeFromHead_, delphiEye_{0, 1.6f, -0.1f}, delphiEyeFromHead_{0, 0, -0.1f};  // per hero (setHero)
    bool meccEyeSaved_ = false;
    Vec3 kabutoEyeModel_{0, 13.0f, -1.0f}, kabutoEyeFromHead_{0, 0, -0.5f};
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
    // Its rockets (weapondefs item 14: 12 carried, 8 with it; ammo item 40, 3 per mc_pickup_rockets).
    int launcherAmmo_ = 0, launcherMax_ = 12, launcherGive_ = 8, launcherPickup_ = 3;
    void giveLauncher(const char* why);
    // weaponSel_: 0 handgun, 1 launcher, 2 bush pack, 3 pop-up bomb, 4 shotgun, 5 machine gun, 6 sniper,
    // 7 Millenium mortar, 8 grenades, 9 mines.
    int weaponSel_ = 0;
    // The Meccaryn's other guns (game_app_arsenal.cpp), from weapondefs.bin: the items the missions
    // hand out (3 shotgun from 1c, 5 machine gun, 6 sniper gun from story3_bunker_3, 7 the Millenium
    // mortar from 4a), each with its
    // model's Ref_handle / Ref_nozzle01, its weapon record (damage, rate, speed) and ammo.
    enum { kGunShotgun = 0, kGunMachine, kGunSniper, kGunMortar, kNumGuns };
    struct Gun {
        const char* name = "";
        int item = -1, sel = -1, hudLayer = -1;
        const RenderModel* model = nullptr;
        Vec3 handle, muzzle;
        float damage = 1.0f;     // x the handgun's
        float interval = 0.5f, speed = 70.0f, range = 250.0f;  // s, m/s, m
        int maxAmmo = 30, startAmmo = 10, pickupAmmo = 5;
        bool have = false;
        int ammo = 0;
        float cool = 0;  // s until it can fire again
    };
    Gun guns_[kNumGuns];
    // Held-weapon state that only the hero's weapon update refreshes: dropped on any frame it didn't
    // run (death, riding, menus, cutscenes, loading, another hero) - the machine-gun loop, the scope
    // pass, Delphi's drawn bow.
    bool weaponsTicked_ = false;
    void dropHeldWeaponState();
    WeaponDefs weaponDefs_;
    bool arsenalReady_ = false;
    void initArsenal();
    void grantMissionItems(const std::vector<uint32_t>& items, const char* why);
    void grantItem(uint32_t item);
    void resetArsenal();
    int gunOfSel(int sel) const;  // kGun* for a weaponSel_, -1 for the others
    bool weaponOwned(int sel) const;
    const RenderModel* heldModel() const;
    void heldRefs(Vec3& handle, Vec3& muzzle) const;
    // Fires the selected gun (shotgun / machine gun / sniper); false if it is not one of them.
    bool fireGun(float dt, bool trigger, bool valid, const Vec3& muzzle, const Vec3& fwd);
    bool ammoPickup(int objectType, const Vec3& at);  // false: not an ammo pickup
    std::vector<uint32_t> missionItems(const std::string& mission) const;
    struct AmmoPickup { Vec3 pos; int type; float t = 0; };
    std::vector<AmmoPickup> ammoPickups_;
    const RenderModel* shellsModel_ = nullptr;
    std::unordered_map<int, const RenderModel*> ammoModels_;  // by pickup type (its object's model)
    const RenderModel* ammoModel(int type);
    // A killed Reaper's ammo (ours: the original's pickups, for a weapon carried that has room).
    bool dropAmmo(const Vec3& at);
    const RenderModel* lbombModel_ = nullptr;  // the mortar's Millenium bomb (mc_lbomb)
    const RenderModel* sniperShotModel_ = nullptr;
    const RenderModel* sniperFlashModel_ = nullptr;
    const RenderModel* shellModel_ = nullptr;
    bool gunTriggerHeld_ = false;
    uint32_t mgLoop_ = 0;  // the machine gun's firing loop
    void updateAmmoPickups(float dt);
    void renderAmmoPickups();
    // Delphi's bows (game_app_bow.cpp): weapondefs items 44 bow (rp_bow), 45 RPG bow (rp_bow_rpg), 46 sniper bow
    // (rp_bow_sniper), handed out in r1_smartiegrab (45) and r4a (44 45 46). Right stick forward switches
    // sword <-> bows. Draw with the other hand (grab the string and pull) or hold the trigger; let go to shoot.
    enum { kBowPlain = 0, kBowRpg, kBowSniper, kBowHoming, kBowPowerup, kNumBows };
    static constexpr int kDelphiMine = kNumBows;  // delphiWeapon_: her mines (item 63 rp_mine)
    bool delphiBow() const { return delphiWeapon_ >= 0 && delphiWeapon_ < kNumBows; }
    struct Bow {
        const char* name = "";
        int item = -1;
        const RenderModel* model = nullptr;
        bool have = false;
        float damage = 2.5f, interval = 1.0f;  // full draw, x the handgun's
        int ammo = -1, maxAmmo = -1, startAmmo = 0;  // -1: no ammo needed
    };
    Bow bows_[kNumBows];
    int delphiWeapon_ = -1;  // -1 the sword, a bow, or kDelphiMine
    bool bowsReady_ = false, bowValid_ = false, bowDrawing_ = false, bowByHand_ = false, bowSwitchArmed_ = true, bowTrigWas_ = false, bowGripWas_ = false;
    float bowDraw_ = 0, bowCooldown_ = 0, bowScale_ = 0.75f;
    Mat4 bowXf_ = Mat4::identity();
    Vec3 bowTips_[2], bowNockRest_, bowNock_, bowFwd_, bowGrip_;
    QuadBatch bowLines_;
    bool bowLinesInit_ = false, bowLinesShown_ = false;
    const RenderModel* arrowModel_ = nullptr;
    void initBows();
    bool grantBowItem(uint32_t item);
    void updateBow(float dt, const InputFrame& in);
    void buildBowLines();
    void renderBow();
    // The sniper gun's zoom: its side screen (the model's third part, beside the "screen" node) shows
    // the view down the barrel at 4x, rendered offscreen while the gun is held close to the eye.
    bool scopeWanted_ = false, scopeShown_ = false, scopeInit_ = false;
    Vec3 scopeEye_, scopeFwd_;
    Vec3 scopeCorners_[4];
    QuadBatch scopeBatch_;
    void updateScope(const Vec3& muzzle, const Vec3& fwd, const Vec3& eye);
    void renderScope();
    void drawScopeLens();
    void scopeCross();
    // Grenades (item 8; game_app_grenade.cpp): thrown by hand - hold the trigger, swing, let go.
    int grenades_ = 0, grenadeMax_ = 8, grenadeGive_ = 5;
    const RenderModel* grenadeModel_ = nullptr;
    Vec3 grenHandPrev_, grenHandVel_;
    Vec3 grenVelHist_[10];  // the hand's last frames' velocities: a throw uses the fastest (release comes late)
    int grenVelAt_ = 0;
    bool grenHandValid_ = false, grenCocked_ = false;
    void updateGrenades(float dt, const InputFrame& in);
    void renderGrenades();
    // Mines (item 9; game_app_mine.cpp), weaponSel_ 9: set down, armed after 2 s, set off by foes.
    int mines_ = 0, minesMax_ = 5, minesGive_ = 5;
    const RenderModel* mineModel_ = nullptr;
    const RenderModel* rpMineModel_ = nullptr;  // Delphi's (item 63: object 1222 rp_mine, the same ammo, item 9)
    struct PlacedMine { Vec3 pos; float yaw; float t; bool armed; bool rp = false; };
    bool holdingMine() const;
    std::vector<PlacedMine> placedMines_;
    bool mineTrigWas_ = false;
    void initMines();
    void updateMines(float dt, const InputFrame& in);
    void renderMines();
    bool switchArmed_ = true;
    float rocketCooldown_ = 0;
    bool rocketTriggerHeld_ = false;
    // kind 0 rocket, 1 Delphi's fire bolt, 2 a SAM's homing missile, 3 a Reaper's bazooka rocket, 4 a shotgun shell,
    // 5 a Millenium bomb, 6 a grenade, 7 Delphi's RPG-bow arrow. dmg: the direct hit (x the handgun's), 0 = the kind's own.
    struct Rocket { Vec3 pos, vel; float life; int kind = 0; int bounces = 0; float dmg = 0; uint32_t fx = 0; };  // fx: its fx.bin trail (0 none yet, kNoFx none)
    static constexpr uint32_t kNoFx = 0xFFFFFFFFu;
    static bool rocketTrail(const Rocket& r) { return r.fx != 0 && r.fx != kNoFx; }  // the original trail rides it
    void syncRocketFx();
    // Reaper soldiers' weapons (game_app_rpweapons.cpp): 0 spear gun, 1 bazooka.
    struct RpWeapon { const RenderModel* model = nullptr; Mat4 handleInv = Mat4::identity(), nozzle = Mat4::identity(); };
    RpWeapon rpWeapon_[2];
    bool rpWeaponsReady_ = false;
    void initReaperWeapons();
    void placeReaperWeapons();
    bool reaperMuzzle(const Enemy& e, Vec3& muzzle) const;
    void renderReaperWeapons(bool shadow, const Vec3& eye, float range);
    std::vector<Rocket> rockets_;
    void updateRockets(float dt);
    // fire: the big blast visual and sound; hurtsPlayer: -1 = as fire (a quake doesn't throw its maker).
    void explode(const Vec3& at, float radius, float damage, bool fire = true, int hurtsPlayer = -1);
    std::string blastFx_;  // the fx.bin effect the next explode() shows (with the original effects on)
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
        float flash = 0;     // rework: just hit (the body flashes)
        int type = -1;       // object type (hidden while a cutscene shows its own copy)
        int rescue = -1;     // index in rescue_ (1b's stranded Smarties); clipIndex = SmartieClip
        bool pooled = false; // a nest's Ripper model, lent to a hatched Ripper while it lives
        // A mission target that is neither Reaper nor Ripper (death trigger: Offspring in 4b, the
        // Charger in 5a): shootable with the data's hit points; it doesn't fight back yet (ours).
        bool boss = false;
        float hp = 0, bossR = 1, bossH = 2, dieT = -1;
        // The other Kabuto (kstory5_4 / 5_5): walks at the player, punches and stomps (ours).
        bool bossKb = false;
        int bossKind = 0;      // with AI: 1 Kabuto, 2 Offspring, 3 Charger, 4 Sonak (0: a target only)
        Vec3 bossChargeDir;
        int bossClip[6] = {};  // stand, walk, punch, stomp, roar, die
        int bossState = 0;
        float bossT = 0, bossClipT = 0, bossCool = 2, bossMaxHp = 1;
        bool bossHit = false;
        // Flyers (Dactyls): circle their home in the sky, banking into the turn.
        bool flyer = false;
        Vec3 home;
        float flyR = 30, flyH = 15, flySpeed = 9, flyAngle = 0, flyDir = 1, roll = 0, pitch = 0;
        Mat4 xf() const {
            return Mat4::translation(pos + Vec3{0, lift, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yaw)) *
                   Mat4::rotation(Quat::fromAxisAngle({1, 0, 0}, pitch)) * Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, roll)) *
                   Mat4::scale({scale, scale, scale});
        }
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
    // Burrowing Rippers (game_app_ripper.cpp): mounds over the buried ones, the dirt thrown up ahead
    // of one tunnelling, the hole it leaves bursting out, small ones riding the player, and the
    // spit a dead one leaves behind (a health pickup: orange a little, blue a lot).
    std::unique_ptr<SkinnedModel> mound_;
    int moundOpen_ = 0;
    const RenderModel* dirtChunk_ = nullptr;
    struct GroundHole { Vec3 pos, normal; float size, angle, age; };
    std::vector<GroundHole> holes_;
    struct OpenMound { Vec3 pos; float scale, yaw, age; };
    std::vector<OpenMound> openMounds_;
    TextureId holeTex_ = 0;
    QuadBatch holeBatch_;
    struct SpitDrop { Vec3 pos; int big; float age; };
    std::vector<SpitDrop> spitDrops_;
    float burrowTick_ = 0;
    // Jetpack effects (game_app_jet.cpp).
    std::unique_ptr<SkinnedModel> thrustFlame_, thrustNitro_, thrustStar_;
    TextureId jetGlowTex_ = 0;
    QuadBatch jetGlowBatch_;
    Mat4 nozzleXf_;
    Vec3 jetGlowAt_;
    float jetGlow_ = 0, jetFlick_ = 0, jetPuffT_ = 0, jetDustT_ = 0;
    bool jetFlickOn_ = true, jetFxOn_ = false, jetNitro_ = false;
    void initJetFx();
    void updateJetFx(float dt);
    void renderJetFx(bool firstPersonView);
    void initBurrows(AssetWorld* w, ModelCache* mc);
    void burrowEvent(const CreatureEvent& e);
    void updateBurrows(float dt);
    void renderBurrows(const Vec3& eye);
    void addHole(const Vec3& at, float size);
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
        bool small = true;  // the smaller nests hatch mini Rippers, the big ones full-sized
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
    void cutsceneEffect(const std::string& name, const Vec3& at);  // flick op 0x27: the game's effect, else our stand-in
    // The game's own effects from fx.bin (game_app_datafx.cpp, game/fx_player.h).
    FxLibrary fxLib_;
    FxPlayer fxPlayer_;
    bool fxLibLoaded_ = false;
    void initDataFx();
    bool playDataFx(const std::string& name, const Vec3& at, const Quat& rot = Quat{}, float scale = 1.0f);
    void updateDataFx(float dt);
    void renderDataFx();
    bool dataFxOn() const;   // Options -> EFFECTS: ORIGINAL (TEST), or GVR_DATA_FX
    QuadBatch streakBatch_;  // the streaks attached fx.bin emitters leave (rockets, arrows)
    TextureId streakTex_ = 0;
    void buildStreaks(const Vec3& eye);
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
    // kind: 1 reach, 2 kill (a character), 3 rescue, 4 bring Vimp meat, 5 destroy a structure (a death
    // trigger on a building: barracks, prison, door, launcher base; `item` = its destruct_ index).
    struct Objective { int kind; Vec3 boundsMin, boundsMax; bool done = false; int item = -1; };
    std::vector<Objective> objectives_;
    size_t objectivesDone_ = 0;
    bool missionComplete_ = false;
    float cutBlack_ = 0;  // the cutscene's blackout last frame (carried into a fade-in on the game)
    int foveationOverride_ = -1;  // test runs (GVR_FFR) pin it
    bool advanceDone_ = false;  // the step to the next mission has started
    // Loading / briefing screen between missions: fade to black, the mission's title and objectives
    // (GText <mission>, MO_<mission>), load behind it, continue on its own.
    enum class LoadPhase { Idle, FadeOut, Show, Brief, FadeIn };
    struct LoadState {
        LoadPhase phase = LoadPhase::Idle;
        float t = 0, yaw = 0;
        int frames = 0;
        std::string mission, title, brief;
        std::string world;  // non-empty: switch to this world first (end of a story chapter)
        std::string worldMission;  // with `world`: start this mission of it (a checkpoint) instead of the first
    } load_;
    int warmFrame_ = 0;  // warm-up views drawn behind the briefing
    bool warmedThisFrame_ = false;
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
    float perfIgnore_ = 8.0f;  // seconds not counted (start, after loads)
    int perfGood_ = 0;          // full-rate windows in a row (quality back up after 3)
    int perfFrames_ = 0;
    // Each world starts at the level it needed last time (quality.txt next to the settings), else at
    // the level measured on the Quest 2 (Kabuto 3 and Reaper 2 need 2); a step back up that had
    // to be undone within 10 s holds the level for the next 60 s (no see-sawing between two levels).
    std::string qualityWorld_;
    std::unordered_map<std::string, int> worldQuality_;
    float sinceUp_ = 1e9f, upHold_ = 0;
    void loadWorldQuality();
    void saveWorldQuality() const;
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
    // Name tags and Smartie lines (game_app_tags.cpp).
    std::unordered_map<uint64_t, float> tagAlpha_;
    float rideLineT_ = 10.0f;
    double tagTime_ = 0;
    std::string smartieName(uint32_t seed) const;
    void smartieSay(const std::string& name, const char* key);
    void updateSmartieLines(float dt);
    void renderTags();
    // The Shop (game_app_shop.cpp).
    struct ShopItem { int kind; Vec3 pos; bool taken; };  // kind 0 rocket launcher, 1 health injection, 2 flares, 3 pop-up bomb, 4 ammo, 5 mines, 6 homing bow, 7 power-up bow
    std::vector<ShopItem> shopItems_;
    Vec3 shopAt_, shopCentre_;
    bool shopOn_ = false, shopInside_ = false, shopAnchored_ = false;
    float shopPool_ = 0, shopPoolSnd_ = 0, shopFxT_ = 0, shopRingT_ = 0;
    void initShop();
    // The pop-up bomb (game_app_popup.cpp): weapon strip choice weaponSel_ 3.
    struct Popup { Vec3 pos; float yaw = 0, t = 0; bool fired = false; };
    std::vector<Popup> popups_;
    int popupBombs_ = 0;
    bool popupBlast_ = false;
    std::unique_ptr<SkinnedModel> popupModel_;
    int popupClip_[3] = {0, 0, 0};
    ButtonEdge popupEdge_;
    void initPopup();
    void updatePopup(float dt, const InputFrame& in);
    void renderPopup();
    void givePopupBomb(int n, const char* why);
    bool popupOnlyTarget(int item) const;
    void openShopAt(const Vec3& centre);
    // Base building (game_app_base.cpp): 5c's Meccaryn base from Mecc_base_template_1.
    struct Base {
        bool on = false, complete = false, onPad = false, autoOrdered = false, reaper = false;
        // What the finished buildings do (ours, after GTextEnglish's descriptions): the Party House's
        // "special items" in the shop; the Pub / Persuader keep the Smarties working (wrecked: "Can't
        // build - Repair the Pub!!!"); the Persuader "helps them work, so long as you keep feeding it
        // souls"; the Shield Generator "gives extra shielding to all of your buildings".
        bool partyGoods = false;        // the Party House stood: its goods go into the shop (now or when it opens)
        float soulT = 0;                // the Persuader's appetite (one soul every 30 s while it works)
        float blockSayT = 0;
        Vec3 centre, padAt, dropAt, menuAt;
        float menuYaw = 0, touchCool = 0, traceT = 0, speed = 1.0f;
        // Raids (ours): Reapers sent from the enemy barracks' side to wreck the buildings.
        float raidT = -1;         // s until the next raid (< 0: not started)
        int raids = 0;
        struct Raider {
            uint32_t uid; int route = -1; size_t next = 0; int route0 = -1; bool snatch = false, back = false; Vec3 escape;
            Vec3 lastPos; float stuckT = 0;  // progress check (stuck raiders get past, ours)
            size_t from = 0;                  // path index it set out from: carried back past it, the leader is lost
        };
        std::vector<Raider> raiders;
        // The missions' army_spline paths (barracks -> base), runtime points; those ending near the base.
        std::vector<std::vector<Vec3>> routes;
        bool routesRead = false;
        // The Smartie workforce (game_app_basecrew.cpp).
        struct Worker {
            Vec3 pos;
            float yaw = 0, lift = 0, clipT = 0;
            int clip = 0, site = -1;  // site: index into bldgs being worked on
            bool joined = false;      // reached the base (counts as a worker)
            std::unique_ptr<SkinnedModel> model;
        };
        std::vector<Worker> crew;
        struct Hatch {
            Vec3 pos;
            float yaw = 0;
            bool open = false;
            float openT = 0;                       // s since it opened (sm_hatch_open plays once)
            std::unique_ptr<SkinnedModel> model;   // Sm_hatch_l0 (the placed static copy is hidden)
            float posedT = -1;
        };
        std::vector<Hatch> hatches;
        std::vector<std::unique_ptr<SkinnedModel>> crewPool;  // the hatches' Smarties, loaded behind the loading screen
        int crewClip[16] = {};
        // The base's leader - Borjoyzee (5c) / Yan (r4a) - the "flag Smartie": raiders try to carry
        // him off to their barracks ("If he gets kidnapped and taken back to the enemy base, you lose!").
        std::unique_ptr<SkinnedModel> leader;
        int leaderClip[4] = {0, -1, -1, -1};  // stand, squirm (carried), walk, wave
        Vec3 leaderHome, leaderPos;
        float leaderYaw = 0, leaderLift = 0, leaderT = 0;
        uint32_t carrier = 0;  // raider uid carrying him (0: none)
        float failT = -1;      // > 0: mission failed, restarting
        std::string leaderName;
        Vec3 raidFrom;
        bool haveRaidFrom = false;
        float attackSayT = 0;
        int food = 0;  // Vimp meat in the store
        struct Bldg {
            int def = 0;
            int state = 0;  // 0 not ordered, 1 building, 3 complete
            float progress = 0, fxT = 0;
            Mat4 xf = Mat4::identity();
            std::unique_ptr<SkinnedModel> model;
            const RenderModel* still = nullptr;  // models without a skin
            int collision = -1;
            float hp = 0, maxHp = 0;  // once standing: raiders wear it down (ours)
            int level = 0;            // 1: upgraded (stone / level 2)
            float posedT = -1;        // clip time of the current pose (re-posed only when it changes)
            float hitT = -1e9f;       // last damage time (sign shows its health a while)
            // Below half health the data's damaged model (D_<model>: scorched, broken) stands instead.
            const RenderModel* damaged = nullptr;
            // Gates: the data's <gate>_open clip plays while you're near (shut, they're solid).
            int openClip = -1;
            float openT = 0;          // s into the open clip (0 shut)
            // Turrets (ours): the head turns to the nearest Reaper and fires.
            float aimYaw = 0, fireT = 0, posedYaw = 1e9f, posedOpen = -1;
            uint32_t target = 0;
        };
        std::vector<Bldg> bldgs;
        struct TurretShot { Vec3 pos, vel; float life; uint32_t target; };
        std::vector<TurretShot> turretShots;
        struct Item { int def; Vec3 pos; };
        std::vector<Item> items;
        std::unique_ptr<SkinnedModel> firepit;
        Mat4 firepitXf = Mat4::identity(), dropXf = Mat4::identity();
        const RenderModel* dropModel = nullptr;
        std::unordered_map<int, int> rewarded;  // per kind: the highest level whose reward was given (a rebuild gives none)
    };
    Base base_;
    void initBase();
    void updateBase(float dt, const InputFrame& in);
    void renderBase(const Vec3& eye);
    std::vector<int> baseOffers() const;
    int baseKindState(int def) const;
    int baseKindLevel(int def) const;
    void orderUpgrade(int def);
    void orderBuilding(int def);
    void updateRaids(float dt);
    void placeTurretSites(int def, const Mat4& baseXf);
    void updateGates(float dt);
    void updateTurrets(float dt);
    void wreckBuilding(Base::Bldg& b);
    bool addCrewSmartie(const Vec3& at, bool fromHatch);
    std::unique_ptr<SkinnedModel> loadCrewModel();
    void initCrew();
    void updateCrew(float dt);
    void renderCrew(const Vec3& eye, bool shadow);
    void poseCrew();
    int crewCount() const;
    int crewSites() const;
    void initLeader();
    void updateLeader(float dt);
    void updateShop(float dt, const InputFrame& in);
    void renderShop();
    std::vector<Vec3> shopCentres_;
    // Flares (game_app_flare.cpp): bought at the Shop, thrown off the pack with the move-hand
    // trigger; a SAM's homing missile goes for a burning flare instead of the player.
    static constexpr int kMaxFlares = 5;  // weapondefs item 64 IconFlare: 5 carried, 3 at a time (was 6, ours)
    int flares_ = 0;
    struct Flare { Vec3 pos, vel; float age; };
    std::vector<Flare> flareList_;
    uint32_t enemyPoseTick_ = 0;
    ButtonEdge flareEdge_;
    float flareFlash_ = 0;
    void updateFlares(float dt, const InputFrame& in);
    // The bush pack (game_app_bush.cpp): weapon strip item 3 (weaponSel_ 2).
    std::unique_ptr<SkinnedModel> bushModel_;
    int bushIn_ = 0, bushOut_ = 0, bushShake_ = 0, bushClip_[4] = {};
    float bushScale_ = 1, bushT_ = 0, bushOutT_ = -1;
    bool bushOn_ = false;
    bool hasBush() const;
    void initBush();
    void updateBush(float dt, PlayerInput& pi);
    void bushEvents();
    bool bushBodyClip(int& clip, float& t) const;
    void renderBush();
    bool flareTarget(const Vec3& from, Vec3& out) const;
    // The VR map (game_app_map.cpp).
    bool mapOpen_ = false, mapDirty_ = false;
    ButtonEdge mapEdge_;
    MeshId mapMesh_ = 0;
    TextureId dotTex_ = 0;
    QuadBatch mapDots_;
    std::vector<Vertex> mapVerts_;
    std::vector<Vec3> mapBase_;
    std::vector<uint8_t> explored_;
    std::string mapWorld_;
    Vec3 mapMin_, mapMax_, mapOffset_;
    float mapScale_ = 1, mapOpenT_ = 0, exploreT_ = 0;
    void mapReset();
    bool buildMapMesh();
    void recolourMap();
    void updateMap(float dt, const InputFrame& in);
    Mat4 mapTransform() const;
    Vec3 mapPoint(const Vec3& world, float lift) const;
    void buildMapBatch();
    void renderMap();
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
    // game_app_water.cpp: splashes (sound 0 none, 1 a shot, 2 a body), shots into the water,
    // the piranhas' frenzy, the water round the legs.
    float flowT_ = 0, flowRingT_ = 0;
    void waterSplash(const Vec3& at, float size, int sound);
    bool shotIntoWater(const Vec3& p, const Vec3& dir);
    void piranhaBite(const Vec3& at, float size, bool loud);
    void updateWaterFlow(float dt);
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
    Vec3 cutEye_, cutOffset_, cutVigPrev_;
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
    // Playable heroes: the Meccaryn (Baz) in the Mecc story, Delphi (the Sea Reaper, rp_l0) in the
    // Reaper story (Rstory worlds). Delphi has no jetpack: she jumps, runs on turbo, swims, fights
    // with her sword (first person: swing it; third person: the trigger) and casts fire bolts.
    // (game_app_delphi.cpp)
    enum class Hero { Mecc, Delphi, Kabuto };
    Hero hero_ = Hero::Mecc;
    void heroBody(Hero h, PlayerController& pc, float& scale, float& lift) const;
    // Skirmish (game_app_skirmish.cpp): the original's multiplayer maps (w_M_*) against computer players.
    struct SkBody {
        std::unique_ptr<SkinnedModel> model;
        int clip[int(PlayerAnim::Count)] = {};
        int die = -1;
        Hero hero = Hero::Mecc;
        float lift = 0;
        Mat4 xf = Mat4::identity();
        bool handValid = false;
        Vec3 hand;
    };
    Skirmish sk_;
    std::vector<SkBody> skBodies_;  // per skirmish player (no model for the local player)
    bool skirmish_ = false;
    int skLocal_ = -1;
    std::unique_ptr<SkinnedModel> skHolder_[3];  // the flag holders (smborjoyzee_l0)
    int skHolderClip_[3][3] = {};                // per holder: idle, carried by a Meccaryn, by a Reaper
    const RenderModel* skFlag_[3] = {};
    const RenderModel* skBow_ = nullptr;
    float skAnimT_ = 0, skBannerT_ = 0, skRoundT_ = 0;
    std::string skBanner_;
    static constexpr int kBotTarget = 3000;  // weapon target ids for skirmish players (id = target - this)
    // Online (game/sk_net.h): this headset hosts the skirmish or has joined someone's.
    enum class SkNetMode { Off, Host, Join };
    SkNetMode skNet_ = SkNetMode::Off;
    SkNetHost skHost_;
    SkNetClient skClient_;
    int skNetWant_ = 0;            // the menu's choice: 0 computer players, 1 host, 2 join
    int titleSkGame_ = 0;          // the found game chosen to join
    int titleSkAddr_ = 0;          // the host's address typed by hand: its last number (0: not set yet)
    bool titleJoining_ = false;
    int skLastHurtBy_ = -1;
    float skLastHurtAt_ = -1e9f;   // (game time: a fall long after a hit is nobody's kill)
    size_t skShotsSeen_ = 0;
    float skLostT_ = 0;
    int skPrevWinner_ = -1;
    bool skAddBody(int id);        // the model of a player, by its team's hero
    struct SkBuilding { std::unique_ptr<SkinnedModel> model; const RenderModel* still = nullptr; Mat4 xf = Mat4::identity(); bool pad = false; };
    std::vector<SkBuilding> skBuildings_;  // the sides' bases, standing (game_app_base.cpp standingBase)
    int standingBase(bool reaper, const Mat4& at, std::vector<SkBuilding>& out);
    void addPartyGoods();
    int skAddRemote(const std::string& name, int team);
    void skOwnShots();             // the local player's new shots, for the others to see
    void skHandleEvent(const SkEvent& e);
    void initSkirmish();
    Vec3 skFlagSpot(const Vec3& pad, const Vec3& toward) const;  // where a side's flag stands: the pad, or the nearest open spot by it
    void updateSkirmish(float dt);
    void renderSkirmish(bool shadow, const Vec3& eye);
    void renderSkirmishHud();
    bool skirmishRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& target) const;
    void skirmishBlast(const Vec3& at, float radius, float damage);
    void skirmishHit(int id, float amount, const Vec3& at, const Vec3& dir);
    std::string skName(int id) const;
    std::string skText(const char* key, const char* fallback) const;
    std::unique_ptr<SkinnedModel> meccMain_, meccSpare_, delphiModel_;
    int meccClip_[int(PlayerAnim::Count)] = {}, delphiClip_[int(PlayerAnim::Count)] = {};
    float meccOneShot_[int(PlayerAnim::Count)] = {}, delphiOneShot_[int(PlayerAnim::Count)] = {};
    PlayerParams meccParams_;
    bool meccParamsSaved_ = false;
    const RenderModel* swordModel_ = nullptr;
    Mat4 swordXf_ = Mat4::identity();
    bool swordValid_ = false, swordTipValid_ = false, swipeTrigWas_ = false, boltTrigWas_ = false, swipeHit_ = false;
    Vec3 swordTipPrev_;
    // Rework: deflect enemy bolts with a blade from hilt to tip (game_app_parry.cpp); returns how many.
    int parryShots(const Vec3& hilt, const Vec3& tip, const Vec3& tipVel, int hand, float dt);
    int parries_ = 0;
    // Delphi's sword trail (game_app_parry.cpp).
    struct SwordTrailPt { Vec3 inner, outer; float age, strength; };
    std::vector<SwordTrailPt> swordTrail_;
    Vec3 swordTrailTip_;
    bool swordTrailValid_ = false;
    QuadBatch trailBatch_;
    void updateSwordTrail(const Vec3& hilt, const Vec3& tip, float dt);
    void renderSwordTrail();
    Vec3 castHandPrev_, castHandVel_;  // Delphi's casting hand (tracking space): a thrown fire bolt
    bool castHandValid_ = false;
    std::vector<std::pair<int, float>> swordCool_;  // target -> seconds until it can be hit again
    int comboStep_ = 0;
    float comboTimer_ = 0, swipeT_ = -1, boltCooldown_ = 0;
    // Delphi's spells (game_app_spells.cpp): fire wall, bubble, tornado.
    struct FireWall { std::vector<Vec3> seg; Vec3 dir; float age = 0; };
    struct Tornado { Vec3 pos, dir; float age = 0, tick = 0; };
    struct Bubble { Vec3 pos; int target = -1; float age = 0; bool caught = false; };
    std::vector<FireWall> fireWalls_;
    std::vector<Tornado> tornadoes_;
    std::vector<Bubble> bubbles_;
    const RenderModel* fireRoll_ = nullptr;
    const RenderModel* swirl_ = nullptr;
    std::unique_ptr<SkinnedModel> bubbleModel_;
    float spellCharge_ = 0;
    bool spellReady_ = false, tornadoHitting_ = false;
    bool tornadoOwned() const;
    void initSpells();
    bool smartieInAim(const Vec3& from, const Vec3& dir, int& index) const;
    bool updateSpells(float dt, bool trig, const Vec3& from, const Vec3& dir, bool aimOk);
    void stepSpells(float dt);
    void renderSpells();
    float hitMul_ = 1.0f;  // damage multiplier for weapon_.onHit (sword strikes)
    static Hero heroForWorld(const std::string& world);
    void setHero(Hero h);
    bool loadDelphi();
    void updateDelphi(float dt, const InputFrame& in);
    // Kabuto (the Kabuto story, Kstory worlds; game_app_kabuto.cpp): kb_l0, about 15x a
    // Meccaryn. The tracking space is scaled (rig_.comfort.worldScale = his eye height / the
    // user's), so the user IS the giant: head, hands and eyes (IPD) scale together. The HUD,
    // menu and hands are drawn in an unscaled frame anchored at the head (beginUiFrame), so they
    // keep their usual size. Turn-hand trigger: punch (first person also by swinging the fist),
    // move-hand trigger: stomp (shockwave), X: grab and eat (heals), Y: roar (scares).
    std::unique_ptr<SkinnedModel> kabutoModel_;
    int kabutoClip_[int(PlayerAnim::Count)] = {};
    float kabutoOneShot_[int(PlayerAnim::Count)] = {};
    int heroDeath_[3][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}};  // [hero][die, drown, fall]
    float heroScale_ = 1.0f;     // body size / the Meccaryn's (capsule, aim points, camera)
    float kabutoEye_ = 22.0f;    // m: his eye height (Ref_Camera in kb_stand)
    float kabutoFeet_ = 0.0f;    // m: lift so his feet touch the ground
    // Growing by eating (k2a's briefing: "Eat 5 Smarties to reach full strength and size"): his size
    // relative to full (kbGrow_ the target, kbGrowShown_ easing to it); everything that comes from
    // his size - height, eye, reach, speed - follows. Base values from setHero.
    float kbGrow_ = 1.0f, kbGrowShown_ = 1.0f, kbBaseScale_ = 1.0f, kbBaseH_ = 1.8f;
    PlayerParams kbBaseParams_;
    void applyKabutoGrowth(float g);
    void kabutoAte(int what);  // 0 Smartie, 1 Vimp, 2 other
    // His story powers (game_app_kbpowers.cpp): growth-gated gates, adrenaline, Offspring.
    float kbAdren_ = 0, kbAdrenT_ = -1, kbGateMsgT_ = 0, kbYHold_ = 0;
    int kbOffspringCharges_ = 0;
    bool kbGripsWas_ = false, kbYLaid_ = false;
    struct OffspringPal {
        std::unique_ptr<SkinnedModel> model;
        int clip[6] = {};
        float h0 = 1, lift0 = 0;
        Vec3 pos;
        float yaw = 0, scale = 1, hatch = 0, life = 0, clipT = 0, cool = 0, retarget = 0, dieT = -1;
        int state = 0, target = -1;
        bool alive = false, struck = false;
    };
    std::vector<OffspringPal> offspring_;
    const RenderModel* eggModel_ = nullptr;
    float gateNeed(int item, bool* needsAdren) const;
    bool kabutoMayDamage(int item);
    void kabutoGain(float adren);
    bool offspringMission() const;
    void initKabutoPowers();
    void kabutoAteSmartie();
    bool layOffspring();
    void updateKabutoPowers(float dt, const InputFrame& in);
    void renderKabutoPowers(const Vec3& eye);
    // The magnifying glass (kstory3_3, kstory4_1).
    bool kbShardOn_ = false, kbHasGlass_ = false, kbGlassAvail_ = false, kbBeaming_ = false;
    Vec3 kbShardAt_, kbGlassAt_, kbGlassDir_{0, 0, -1};
    float kbBeamT_ = 0, glassSize_ = 1;
    const RenderModel* glassModel_ = nullptr;
    void kabutoObjectDestroyed(const std::string& name, const Vec3& at);
    void updateGlass(float dt, const InputFrame& in);
    // Throws (game_app_kbthrow.cpp): boulders off the volcanic rock pillars, and Reapers.
    struct RockGen { Vec3 base, top; float radius = 1, regrow = 0, grow = 1; bool hasTop = true; };
    struct Boulder { enum State { Resting, Loose, Held, Thrown, Gone } state = Resting; Vec3 pos, vel, rot, spin; float age = 0; };
    struct Shrapnel { Vec3 pos, vel, rot, spin; float size = 0.2f, age = 0; };
    struct Thrown { uint32_t uid = 0; float age = 0; };  // a thrown Reaper (by uid: the enemy list shrinks)
    enum { HoldNone, HoldBoulder, HoldReaper };
    std::vector<RockGen> kbRocks_;
    std::vector<Boulder> kbBoulders_;
    std::vector<Shrapnel> kbShrapnel_;
    std::vector<Thrown> kbThrown_;
    int kbHold_ = HoldNone, kbHeld_ = -1;  // kbHeld_: a boulder's index, or the held Reaper's this frame
    uint32_t kbHeldUid_ = 0;               // the held Reaper
    int enemyOfUid(uint32_t uid) const;
    uint32_t targetUid(int target) const;          // eat / hit target ids (enemy, creature, villager, Vimp ranges)
    int targetOfUid(int kindOf, uint32_t uid) const;  // that kind's target with this uid now (-1: gone)
    float kbHoldT_ = 0, boulderSize_ = 1;
    bool kbXWas_ = false, kbHandValid_ = false;
    // Rework: first-person grab with either hand's grip (game_app_kbgrab.cpp). kbGrabHand_: the hand
    // holding kbHold_ (-1: held by X, or nothing).
    int kbGrabHand_ = -1;
    float kbHeadPrevY_ = 0, kbHeadVelY_ = 0, kbLeapCool_ = 0;  // Kabuto's real-jump leap
    bool kbHeadValid_ = false;
    bool kbGripWas_[2] = {false, false};
    void updateKabutoHandGrab(const InputFrame& in);
    void checkReachObjectives();  // test hook GVR_REACH_CHECK (game_app_reach.cpp)
    bool planReach(int objective, bool log);
    bool reachWalkInput(Vec2& move, float& viewYaw, bool& jump, float dt);  // GVR_REACH_WALK: steers along reachPath_
    std::vector<Vec3> reachPath_;
    std::vector<uint8_t> reachBlocked_;
    std::vector<float> reachHgt_;
    Vec3 reachLo_;
    int reachW_ = 0, reachTarget_ = -1, reachReplans_ = 0, reachResets_ = 0, reachLog_ = 0;
    float reachStuckT_ = 0;
    Vec3 kbHandPrev_, kbHandVel_;
    const RenderModel* boulderModel_ = nullptr;
    void initThrows();
    void updateThrows(float dt, const InputFrame& in);
    void renderThrows();
    void rockHit(const Vec3& at, float radius);
    bool boulderInReach() const;
    float boulderRadius() const;
    bool throwAimAt(const Vec3& from, Vec3& target) const;
    Vec3 lobTo(const Vec3& from, const Vec3& to, float elev) const;
    void kabutoEat(int target);
    float heroLift_ = 0.0f;      // m: the current hero's model lift
    struct KabutoState {
        float punchT = -1, stompT = -1, eatT = -1, roarT = -1, roarCool = 0, stepT = 0;
        bool punchHit = false, stompHit = false, eatDone = false, punchLeft = false;
        bool trigWas[2] = {false, false}, xWas = false, yWas = false, wasAir = false;
        Vec3 fistPrev[2];  // tracking space (the user's real hand: teleports, turns and falls aren't swings)
        bool fistValid[2] = {false, false};
        float fistCool[2] = {0, 0};
        int eatTarget = -1;
        uint32_t eatUid = 0;
    } kb_;
    float uiScaleSaved_ = 1.0f;
    Pose uiRigSaved_;
    bool uiFrame_ = false;
    bool loadKabuto();
    void updateKabuto(float dt, const InputFrame& in);
    void kabutoQuake(const Vec3& at, float radius, float damage);
    void updateHeroScale();
    // Draw head-anchored UI unscaled: swap the rig to scale 1 about the head and re-begin the view.
    void beginUiFrame(const EyeRenderInfo* eyes, int n);
    void renderViews(const EyeRenderInfo* eyes, int n);
    // Starts the view(s) for these eyes (n = 2: both at once); worldEyeOverride: all from one pose.
    void beginEyeViews(const EyeRenderInfo* eyes, int n, const Pose* worldEyeOverride, bool clear);
    void endUiFrame();
    // Main menu (game_app_title.cpp): the original's title scene - night over the intro island
    // (w_intro_island), seen from where its menu camera rests (flk_intro_island_2), the 3D GIANTS
    // logo (Giants_logo_3D.gbs) in the sky - with CONTINUE / NEW GAME (story, shown with its
    // loading art) / OPTIONS.
    bool titleWanted_ = false, title_ = false, titleViewSet_ = false, titleActHeld_ = true, titleFirst_ = true;
    std::string titleWorld_, titleMission_;   // the checkpoint CONTINUE goes to ("" none)
    std::vector<std::pair<std::string, std::string>> titleMissions_;  // main menu mission select: (world, mission)
    int titleMissionSel_ = 0;
    void startChosenMission(const InputFrame& in);
    int titleRow_ = 0, titleCampaign_ = 0;
    int titleSkMap_ = 0, titleSkTeam_ = 0;  // MULTIPLAYER: the map and the team chosen
    int skWantTeam_ = -1, skPerTeam_ = 3;   // a skirmish started from the menu (-1: GVR_SKIRMISH / defaults)
    float titleRepeat_ = 0, titleHold_ = 0, titleT_ = 0, titleYaw_ = 0;
    Vec3 titleEye_;
    const RenderModel* logoModel_ = nullptr;
    void enterTitle();
    void updateTitle(float dt, const InputFrame& in);
    void renderTitle();
    void startFromTitle(const std::string& world, const std::string& mission, bool restore, const InputFrame& in);
    TextureId loadingArt(Hero h);
    // Music: the ambient set for a mission (a1/a2 alternate by chapter; ours).
    std::string ambientFor(const std::string& world) const;
    void pushUnscaledRig(const Pose& head, bool headValid);
    void popUnscaledRig();
    // VR feel (game_app_feel.cpp): controller rumble (hits by side, jetpack, landings, a heartbeat
    // when low) and the holstered weapon - gun on the right thigh (Ref_holsterR), launcher across
    // the back (Ref_HolsterB) - which first person can draw by reaching for it and gripping.
    struct RefCache { Mat4 local = Mat4::identity(); bool have = false; };
    RefCache holsterRCache_, holsterBCache_;
    bool bodyRef(const char* ref, const char* parent, RefCache& c, Mat4& outGame);
    bool holsterGun_ = false, holsterLauncher_ = false, feelThrust_ = false, feelWasGround_ = true, holsterGripHeld_ = false;
    Mat4 holsterGunXf_ = Mat4::identity(), holsterLauncherXf_ = Mat4::identity();
    Vec3 holsterGunPos_, holsterBackPos_;
    float feelRumble_ = 0, feelFall_ = 0, feelBeat_ = 0, feelBeat2_ = -1;
    void updateFeel(float dt, const InputFrame& in);
    std::string savePath_;
    // Options (game_app_menu.cpp): the left menu button opens them; the game pauses meanwhile.
    UserSettings settings_;
    std::string settingsPath_;
    bool menuOpen_ = false, menuActHeld_ = false, menuFirst_ = true;
    int menuRow_ = 0, menuCampaign_ = 0;
    float menuRepeat_ = 0, menuNewGame_ = 0, menuYaw_ = 0;
    Vec3 menuPos_;
    ButtonEdge menuEdge_;
    void applySettings();
    void saveSettings();
    void openMenu(const InputFrame& in);
    void updateMenu(float dt, const InputFrame& in);
    void renderMenu();
    float fpsSmooth_ = 72.0f;
    EnvironmentParams envBase_;
    std::string envBaseWorld_;
    float envBaseSea_ = -1e30f, dayPhase_ = 0.08f;
    SaveGame pendingRestore_;
    bool haveRestore_ = false;
    float newGameHold_ = 0;
    bool xHeld_ = false;
    void writeCheckpoint();
    // Smartie villagers in the villages (game_app_village.cpp): ours, see villager.h.
    VillagerDirector villagers_;
    std::vector<std::unique_ptr<SkinnedModel>> villagerModels_;
    std::vector<float> villagerLift_;
    int villagerClip_[int(VillagerClip::Count)] = {};
    size_t villagerShots_ = 0;
    void initVillagers(AssetWorld* w, ModelCache* mc);
    void updateVillagers(float dt);
    void renderVillagers(bool shadow, const Vec3& eye, float range);
    // Vimp herds and Vimp meat (game_app_vimp.cpp): ours, see vimp.h. Meat dropped by a killed
    // Vimp is picked up by walking over it; mission trigger kind 4 ("On Vimp Meat Proximity", the
    // original's trigger name) completes when the player brings kVimpMeatNeeded pieces to it.
    static constexpr int kVimpTarget = 4000;   // weapon target ids for Vimps (index = id - this)
    static constexpr int kVimpMeatNeeded = 20; // "Return 20 PIECES OF VIMP MEAT" (2a's briefing)
    VimpDirector vimps_;
    std::vector<std::unique_ptr<SkinnedModel>> vimpModels_;
    float vimpLift_ = 0;
    int vimpClip_[int(VimpClip::Count)] = {};
    size_t vimpShots_ = 0;
    struct Meat { Vec3 pos, vel; float yaw = 0, spin = 0, t = 0; int model = 0; bool rest = false; };
    std::vector<Meat> meat_;
    const RenderModel* meatModel_[3] = {};
    TextureId meatIcon_ = 0, dropoffTex_ = 0;
    int vimpMeat_ = 0;
    float meatFlash_ = 0, meatHintT_ = 0;
    void initVimps(AssetWorld* w, ModelCache* mc);
    void updateVimps(float dt);
    void renderVimps(bool shadow, const Vec3& eye, float range);
    bool vimpRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& index) const;
    void hitVimp(int i, float amount, const Vec3& at, const Vec3& dir);
    void dropMeat(const Vec3& at, int pieces);
    static constexpr int kBossTarget = 8000;  // weapon target ids for boss characters (index into chars_)
    bool bossRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& index) const;
    void hitBoss(int i, float amount, const Vec3& at, const Vec3& dir);
    void updateBosses(float dt);
    // SAM turrets (guard_sam, class 44): "detect airborne targets and hammer them with homing
    // missiles" (2b's briefing). Ours: a missile every few seconds at a flying player in range.
    std::vector<std::pair<int, std::unique_ptr<SkinnedModel>>> towerRigs_;
    std::vector<float> lastHitT_;  // destruct item -> time of its last hit (health bars)
    void renderHealthBars();  // destruct item -> preloaded collapse rig
    std::vector<std::pair<int, float>> samCool_;  // destruct_ item -> seconds to the next launch
    void updateSams(float dt);
    bool spawnRivalKabuto(const Vec3& pos, float yaw);
    int spawnBoss(int kind, const char* model, const char* idle, const Vec3& pos, float yaw, float hp);  // index in chars_
    // Gives a placed character the boss AI of its kind (clips loaded into its model).
    void setupBossAI(CharacterInst& ci, int kind);
    // Reaperski (game_app_jetski.cpp, see jetski.h): Delphi rides a jetski - a parked one she
    // walks into (rstory2_jetski_*), or from the start in the races; the races run on the data's
    // gates and racing lines against rivals.
    bool riding_ = false, raceMode_ = false, skiParked_ = false;
    JetskiState ski_;
    JetskiParams skiParams_;
    RaceDirector race_;
    std::unique_ptr<SkinnedModel> skiModel_;
    std::vector<std::unique_ptr<SkinnedModel>> rivalSki_, rivalRider_;
    int rideClip_[3] = {-1, -1, -1};  // Delphi: rp_Jetski_ride, rp_jetski_L_turn, rp_jetski_R_turn
    int rivalRideClip_ = 0;
    struct SkiPickup { Vec3 pos; int kind; float gone = 0; };  // kind 0 turbo, 1 missile, 2 homing
    std::vector<SkiPickup> skiPickups_;
    const RenderModel* skiPickupModel_[3] = {};
    int skiMissiles_ = 0, raceObjective_ = -1, raceShownPlace_ = 0;
    float skiTurbo_ = 0, raceEndT_ = -1, skiSplashT_ = 0, skiFireCool_ = 0;
    bool skiYWas_ = false, skiTrigWas_ = false, raceWon_ = false;
    uint32_t skiLoop_ = 0;
    int skiLoopKind_ = -1;
    std::string raceMsg_;
    float raceMsgT_ = 0;
    void initJetski();
    void mountJetski(const Vec3& pos, float yaw);
    void updateRiding(float dt, const InputFrame& in);
    void updateJetskiWorld(float dt, const InputFrame& in);
    void renderJetski(bool shadow, const Vec3& eye);
    void renderRaceHud();
    void creatureSound(const CreatureEvent& e);
    void updateCombat(float dt);
    void damagePlayer(float amount, DeathCause cause = DeathCause::Shot);
    void renderHud();
    // HUD with the original Meccaryn HUD art (game_app_hud.cpp): the heart-shaped health gauge,
    // the jetpack's fuel gauge, health injections carried, the weapon in hand and objective pips
    // (FP: over the move-hand wrist; TP: head-locked, lower left); the weapon strip that pops up on
    // a weapon switch; a red marker toward whatever hurt the player; green flash on healing.
    enum HudLayer { kHudPlate, kHudHealthFill, kHudFuelFill, kHudEnergy, kHudJetGauge, kHudWpSlot, kHudGun, kHudLauncher,
                    kHudSword, kHudFire, kHudSyringe, kHudMeat, kHudFlare, kHudBush, kHudNumbers, kHudMarks, kHudPop, kHudShotgun, kHudMachine, kHudSniper, kHudMortar, kHudGrenade, kHudMine, kHudLayers };
    struct HudFrame { Vec3 o, r, u; };  // panel origin and its right / up axes (world, metres per unit)
    TextureId hudTex_[kHudLayers] = {};
    float digitU_[11] = {}, digitU0_[10] = {}, digitU1_[10] = {};
    bool tpGunCal_ = false;     // third person: the gun's pose in the hand bone's frame (from the idle pose)
    Vec3 tpGunFwd_, tpGunUp_;
    bool digitsMeasured_ = false;
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
    static constexpr int kMaxSyringes = 3;  // weapondefs item 0 IconHealthSyringe: 3 carried (was 5, ours)
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
    // Rework, VR feel: the gun kicks up and back in the hand on each shot (spring back; shots still
    // leave along the aim), and a long gun is steadied by the other hand: grip near its front and
    // it aims along the line between your hands.
    float gunKick_ = 0, gunKickV_ = 0;
    bool twoHand_ = false;
    void gunKick(float strength);
    // Light flashes (rework): muzzle flashes and explosions light their surroundings for a moment
    // (the renderer's one point light goes to the brightest; the jetpack keeps it otherwise).
    struct Flash { Vec3 pos, color; float radius, t, dur; };
    std::vector<Flash> flashes_;
    void addFlash(const Vec3& pos, const Vec3& color, float radius, float dur) { if (flashes_.size() < 16) flashes_.push_back({pos, color, radius, 0.0f, dur}); }
    void updateFlashes(float dt);
    MeshId markerMesh_ = 0;
    // Aim laser (rework; Options): a thin red beam from the gun's muzzle to what it points at.
    MeshId laserMesh_ = 0;
    Vec3 laserFrom_, laserTo_;
    bool laserOn_ = false;
    int laserTarget_ = -1;
    float laserOnT_ = 0, dodgeCool_ = 0;
    void renderAimLaser();
    // Floating motes (rework, atmosphere): dust in the sunlight by day, fireflies at night, drifting
    // within ~9 m of the eyes - gives the air depth in VR.
    struct Mote { Vec3 pos, vel; float size, phase; };
    std::vector<Mote> motes_;
    QuadBatch moteBatch_;
    TextureId moteTex_ = 0;
    void updateMotes(float dt);
    std::vector<float> creatureHp_;  // last frame's creature health (hit flashes)
    void updateCreatureFlashes(float dt);
    // Explosion punch (rework): a fireball that swells and fades, and embers thrown out.
    struct Fireball { Vec3 pos; float radius, t; };
    struct Ember { Vec3 pos, vel; float t, life, size; };
    std::vector<Fireball> fireballs_;
    std::vector<Ember> embers_;
    void addSparks(const Vec3& at, const Vec3& normal, int n);
    void updateNearMisses(float dt);
    void updateHandTouch(float dt);
    void drawPanelEdge(const Mat4& xf, float w, float h);
    // Climbing (game_app_climb.cpp).
    void updateClimb(float dt, const InputFrame& in);
    bool climbGripWas_[2] = {false, false}, climbHold_[2] = {false, false};
    Vec3 climbAnchor_[2];
    float climbT_ = 0, climbStuckT_ = 0;
    bool touch_[2] = {false, false};
    float touchT_[2] = {0, 0};
    QuadBatch fireBatch_;
    void addFireball(const Vec3& at, float radius);
    void updateFireballs(float dt);
    void renderFireballs();
    TextureId glowTexture();
    void renderMotes();
    // Grass tufts near the player (game_app_grass.cpp).
    void initGrass();
    void updateGrass();
    void renderGrass();
    bool grassOn_ = false;
    float grassRadius_ = 0;
    const AssetWorld* grassWorld_ = nullptr;
    Vec3 grassCentre_{1e9f, 0, 1e9f}, grassColor_{0.3f, 0.4f, 0.2f};
    TextureId grassTex_ = 0;
    QuadBatch grassBatch_;
    void renderObjectiveMarker();
    float playerHitTop() const;
    bool handReach(const Vec3& p, float r) const;
    float rpGrenadeCool_ = 0;
    void updateReaperGrenades(float dt);
    float ambushT_ = 0, ambushDue_ = 0;
    std::string ambushMission_;
    bool ambushAllowed_ = false;
    void updateAmbush(float dt);
    // Holsters on the body and punches (game_app_vr.cpp).
    int holsterZone_ = -1;
    float holsterYaw_ = 0;
    bool holsterGrabHeld_ = false;
    float punchCool_[2] = {0, 0};
    Vec3 punchLast_[2];
    bool punchHave_[2] = {false, false};
    void updateHolsters(float dt, const InputFrame& in);
    void updateMelee(float dt, const InputFrame& in);
    // Comfort vignette (rework): the view's edges darken while the stick moves or turns you.
    float vignette_ = 0;
    MeshId vignetteMesh_ = 0, hurtRingMesh_ = 0;
    void drawEyeRing(MeshId& mesh, float k, const Vec3& rgb, float alpha);
    void updateVignette(float dt, const InputFrame& in);
    void renderVignette();
    void updateGunKick(float dt);
    Mat4 gunDrawXf() const;
    void drawModel(const RenderModel* m, const Mat4& xf, bool additive, float alpha = 1.0f);
    float camYaw_ = 0;      // play-mode camera yaw (snap turn orbits the character)
    float camFloor_ = 0;    // smoothed tracking-floor height of the play camera
    ButtonEdge playEdge_, viewEdge_;
    bool showTestScene_ = true;
    bool needSpawn_ = false;
    double time_ = 0.0;
};

}  // namespace gvr
