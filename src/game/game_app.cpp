#include "game/game_app.h"
#include "game/hero_clips.h"

#include "core/log.h"
#include "core/prof.h"
#include "scene/asset_world.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "assets/gtext.h"
#include "assets/skin_anim.h"
#include "game/cutscene.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

#include <algorithm>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>

namespace gvr {


// Weapon target ids >= this are creatures (index = id - kCreatureTarget); lower ids are Reapers.
static constexpr int kCreatureTarget = 1000;

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
    dataDir_ = dataDir;
    worldName_ = worldName;
    // Continue from the saved checkpoint (its world and mission), unless a test picks a mission.
    if (!savePath_.empty()) {
        const size_t slash = savePath_.find_last_of("/\\");
        settingsPath_ = (slash == std::string::npos ? std::string(".") : savePath_.substr(0, slash)) + "/settings.txt";
        if (loadSettings(settingsPath_, settings_)) LOGI("Settings loaded from %s", settingsPath_.c_str());
        progressPath_ = (slash == std::string::npos ? std::string(".") : savePath_.substr(0, slash)) + "/progress.txt";
        loadWorldQuality();
    }
    loadProgress();
    if (const char* ov = std::getenv("GVR_SETTINGS")) { applySettingsList(ov, settings_); LOGI("Settings from GVR_SETTINGS: %s", ov); }
    if (!savePath_.empty()) {
        baseSavePath_ = savePath_;
        savePath_ = slotPath(settings_.saveSlot);
        if (settings_.saveSlot != 0) LOGI("Save slot %d: %s", settings_.saveSlot + 1, savePath_.c_str());
    }
    firstPerson_ = !settings_.thirdPerson;
    applySettings();
    std::string startMission;
    if (!savePath_.empty() && !std::getenv("GVR_MISSION") && loadSaveGame(savePath_, pendingRestore_)) {
        haveRestore_ = true;
        worldName_ = pendingRestore_.world;
        startMission = pendingRestore_.mission;
        LOGI("Continuing from the checkpoint: %s / %s", worldName_.c_str(), startMission.c_str());
    }
    // The main menu: the title scene's world first; the checkpoint waits for CONTINUE.
    if (titleWanted_ && !std::getenv("GVR_MISSION")) {
        titleWorld_ = haveRestore_ ? worldName_ : std::string();
        titleMission_ = haveRestore_ ? startMission : std::string();
        worldName_ = "intro_island";
        startMission.clear();
        title_ = true;
    }
    // Test hooks for GPU experiments: GVR_SHADOWS=0 / GVR_REFLECT=0 turn the sun shadow map / the
    // water reflection pass off; GVR_SHADOW_SIZE=1024 etc.
    const char* shEnv = std::getenv("GVR_SHADOWS");
    const char* rfEnv = std::getenv("GVR_REFLECT");
    const char* shSize = std::getenv("GVR_SHADOW_SIZE");
    shadowsOn_ = !(shEnv && shEnv[0] == '0') && renderer_.initShadows(shSize ? std::max(256, std::atoi(shSize)) : 2048);  // 2048: sharper (device test 19)
    reflOn_ = !(rfEnv && rfEnv[0] == '0') && renderer_.initReflection(1024, 768);
    assetWorld_ = std::make_unique<AssetWorld>();
    if (title_ && !assetWorld_->init(dataDir, renderer_, worldName_, startMission)) {
        // No title scene in the data: straight into the game as before.
        LOGW("Main menu world unavailable; starting the game");
        title_ = false;
        worldName_ = haveRestore_ ? titleWorld_ : worldName;
        startMission = titleMission_;
        assetWorld_ = std::make_unique<AssetWorld>();
    }
    if (!title_ && !assetWorld_->init(dataDir, renderer_, worldName_, startMission) && haveRestore_) {
        // A save naming a world that won't load: start over in the default world.
        LOGW("Saved world %s unavailable; starting %s", worldName_.c_str(), worldName.c_str());
        haveRestore_ = false;
        worldName_ = worldName;
        assetWorld_ = std::make_unique<AssetWorld>();
        assetWorld_->init(dataDir, renderer_, worldName_);
    }
    if (!assetWorld_->loaded()) {
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
        // Two variants of the same Meccaryn: with the jetpack on his back (mc_l0) and without
        // (mc_nothrust_l0, same skeleton and clips); the one in use follows whether the player has
        // the jetpack (1a: Baz lost it, Timmy hands it over).
        auto makePlayer = [this](const char* base, bool first) -> std::unique_ptr<SkinnedModel> {
        auto m = std::make_unique<SkinnedModel>();
        if (!m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, base, "mc_stand.anm")) return nullptr;
        {
            const char* const* clips = kMeccClips;
            for (int i = 1; i < int(PlayerAnim::Count); ++i) clip_[i] = std::max(0, m->addAnimation(assetWorld_->fs(), clips[i]));
            if (first) {
                for (int i = 0; i < int(PlayerAnim::Count); ++i) LOGI("Player clip %d %s: %.2f s", i, clips[i], m->animDuration(clip_[i]));
                // Landings and the injection play once. The stun lasts as long as the hard landing.
                for (PlayerAnim a : {PlayerAnim::Land, PlayerAnim::LandHard, PlayerAnim::Inject})
                    player_.oneShot[int(a)] = m->animDuration(clip_[int(a)]);
                if (player_.oneShot[int(PlayerAnim::LandHard)] > 0.5f) player_.params.stunSeconds = player_.oneShot[int(PlayerAnim::LandHard)];
                if (player_.oneShot[int(PlayerAnim::Land)] > 0.2f) player_.params.landSeconds = player_.oneShot[int(PlayerAnim::Land)];
            }
            if (const char* ic = std::getenv("GVR_IDLE_CLIP")) clip_[0] = std::max(0, m->addAnimation(assetWorld_->fs(), ic));  // test hook
            // Death clips: falling dead, going under in the water.
            const int dieA = m->addAnimation(assetWorld_->fs(), "mc_die.anm");
            const int dieB = m->addAnimation(assetWorld_->fs(), "mc_drowning_die.anm");
            const int dieC = m->addAnimation(assetWorld_->fs(), "mc_land_dead.anm");
            if (first) { deathClip_[0] = dieA; deathClip_[1] = dieB; deathClip_[2] = dieC; }
            // The bush pack (2b on): crouching into it, in it, shuffling, out.
            const char* bush[4] = {"mc_bush_in.anm", "mc_bush_idle.anm", "Mc_bush_walk.anm", "mc_bush_out.anm"};
            for (int k = 0; k < 4; ++k) {
                const int bi = m->addAnimation(assetWorld_->fs(), bush[k]);
                if (first) bushClip_[k] = bi;
            }
            // Eye point from the idle pose: at the front of the face (-Z), so looking down shows
            // the chest and arms instead of the inside of the collar armour.
            m->pose(renderer_, 0.0f);
            Vec3 hmn, hmx;
            if (first && m->boneVertexBounds("head", hmn, hmx))
                eyeModel_ = {0.5f * (hmn.x + hmx.x), hmn.y + 0.55f * (hmx.y - hmn.y), hmn.z - 0.04f};
            if (first) LOGI("Player eye point (model, m): %.2f %.2f %.2f", eyeModel_.x, eyeModel_.y, eyeModel_.z);
            if (first) {
                // Timmy's mount (see updateTimmy): the spine's idle rotation, undone so he sits upright
                // when Baz stands; and Ref_Smartie relative to the spine.
                Mat4 sp, rf;
                if (m->nodeWorld("spine", sp)) {
                    Mat4 rt = gameMatrixToRuntime(sp);
                    for (int c = 0; c < 3; ++c) {
                        const float l = std::sqrt(rt.m[c * 4] * rt.m[c * 4] + rt.m[c * 4 + 1] * rt.m[c * 4 + 1] + rt.m[c * 4 + 2] * rt.m[c * 4 + 2]);
                        if (l > 1e-6f)
                            for (int r = 0; r < 3; ++r) rt.m[c * 4 + r] /= l;
                    }
                    Mat4 inv = Mat4::identity();
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) inv.m[c * 4 + r] = rt.m[r * 4 + c];
                    timmyRestFix_ = inv;
                    if (m->nodeWorld("ref_holsterb", rf)) {
                        smartieFromSpine_ = affineInverse(sp) * rf;
                        haveSmartieLocal_ = true;
                    }
                }
            }
            m->setHiddenBones(SkinnedModel::headBones());
            m->pose(renderer_, 0.0f);
            if (first) {
                eyeFromHead_ = eyeModel_ - m->hiddenCentroid();
                eyeNow_ = eyeModel_;
            }
            // First-person arm IK: the Meccaryn's arms reach for the tracked controllers.
            m->setPoseModifier([this](const AnmAnimation& clip, const std::vector<std::vector<int>>& children,
                                      std::vector<Mat4>& world) {
                if (!playMode_ || (!firstPerson_ && !tpAiming_)) return;
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
                    t.handScale = firstPerson_ ? 0.8f : 1.0f;  // the gloves read oversized at first-person distance
                    // Meccaryn arms are short for human reach (0.49 vs ~0.6 m): x1.3 base length, and
                    // beyond that the arm keeps stretching so the hand stays on the controller
                    // (user request after device test 6: arms should stretch further).
                    t.armStretch = firstPerson_ ? 1.3f : 1.0f;
                    t.maxReachStretch = firstPerson_ ? 1.8f : 1.0f;
                    solveArmIk(world, children, c, t);
                    // Grip closes the hand; the trigger curls the leading finger (Meccaryn: 4 digits).
                    const float g = armTargets_[h].squeeze, tr = armTargets_[h].trigger;
                    float curl[4] = {std::max(g, tr), std::max(g * 0.9f, 0.25f * tr), g, g};
                    if (armTargets_[h].point) curl[0] = 0.0f;                                // finger off the trigger: straight
                    else if (armTargets_[h].triggerRest) curl[0] = std::max(curl[0], 0.3f);  // resting on it
                    // The thumb (digit 4: its base sits nearest the wrist) straight while it is off the buttons and stick.
                    if (armTargets_[h].thumbUp) curl[3] = 0.0f;
                    curlFingers(world, children, c, curl);
                }
            });
        }
        return m;
        };
        auto m = makePlayer("mc_l0", true);
        if (m) {
            playerSpare_ = makePlayer("mc_nothrust_l0", false);
            playerHasJet_ = true;
            // First person hides the head; the whole body still shows in reflections and shadows.
            m->setKeepFull(true);
            if (playerSpare_) playerSpare_->setKeepFull(true);
            {
                auto t = std::make_unique<SkinnedModel>();
                // Riding on the back: sm_fly_idle (a Smartie sitting on a Meccaryn's back, facing
                // backwards), with the sm_fly_yell clips now and then in flight.
                if (t->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "smtimmy_l0",
                            std::getenv("GVR_TIMMY_CLIP") ? std::getenv("GVR_TIMMY_CLIP") : "sm_fly_idle.anm")) {  // test hook
                    for (int k = 0; k < 3; ++k) {
                        const std::string yell = "sm_fly_yell" + std::to_string(k + 1) + ".anm";
                        timmyYell_[k] = t->addAnimation(assetWorld_->fs(), yell);
                    }
                    timmy_ = std::move(t);
                }
            }

            playerModel_ = std::move(m);
            playMode_ = true;
            AssetWorld* w = assetWorld_.get();
            // Weapon models: gun (held, so dynamically lit), bolt, muzzle flash, impact flash.
            ModelCache* mc = assetWorld_->models();
            gunModel_ = mc->get("mc_gun_l0.gbs", "", /*dynamicLit=*/true);
            initJetFx();
            initBush();
            {
                // Grip and muzzle points from the weapons' reference nodes (game units, model frame).
                auto ref = [&](const char* anm, const char* node, Vec3& out) {
                    std::vector<uint8_t> b;
                    AnmAnimation a;
                    if (!assetWorld_->fs().read(anm, b) || !parseAnm(b.data(), b.size(), a)) return;
                    std::vector<Mat4> wm;
                    a.evaluate(0.0f, wm);
                    auto it = a.byName.find(node);
                    if (it != a.byName.end()) out = {wm[size_t(it->second)].m[12], wm[size_t(it->second)].m[13], wm[size_t(it->second)].m[14]};
                };
                ref("mc_gun.anm", "ref_handle", gunHandle_);
                ref("mc_gun.anm", "ref_nozzle01", gunMuzzle_);
                ref("mc_launcher.anm", "ref_handle", launcherHandle_);
                ref("mc_launcher.anm", "ref_nozzle01", launcherMuzzle_);
                ref("MC_repairgun.anm", "ref_handle", repairHandle_);
                ref("MC_repairgun.anm", "ref_nozzle01", repairMuzzle_);
                // Held by its wooden pistol grip (the shaft running down and back to the knob at
                // the rear), not at Ref_handle: that sits on the knuckles modelled over the neck
                // (offline side view of mc_launcher_l0; user report after device test 22).
                launcherHandle_ = {0.0f, 3.85f, -0.45f};
                LOGI("Weapon refs: gun handle (%.2f %.2f %.2f) muzzle (%.2f %.2f %.2f); launcher handle (%.2f %.2f %.2f) muzzle (%.2f %.2f %.2f)",
                     gunHandle_.x, gunHandle_.y, gunHandle_.z, gunMuzzle_.x, gunMuzzle_.y, gunMuzzle_.z, launcherHandle_.x, launcherHandle_.y,
                     launcherHandle_.z, launcherMuzzle_.x, launcherMuzzle_.y, launcherMuzzle_.z);
            }
            launcherModel_ = mc->get("mc_launcher_l0.gbs", "", /*dynamicLit=*/true);
            repairModel_ = mc->get("MC_repairgun.gbs", "", /*dynamicLit=*/true);
            repairRing_ = mc->get("EFX_repair_ring.gbs");
            if (repairModel_ && repairRing_)
                LOGI("Repair gun: model ok, ring %.2f m across", length(repairRing_->boundsMax - repairRing_->boundsMin));
            initPortTurret(mc);
            arsenalReady_ = false;  // this world's model cache: the guns are fetched again (kept: what he carries)
            initArsenal();
            rocketModel_ = mc->get("mc_rocket.gbs", "mc_rocket.anm", /*dynamicLit=*/true);
            boltModel_ = mc->get("mc_bullet.gbs", "mc_bullet.anm");
            muzzleModel_ = mc->get("muzzle_flash.gbs", "muzzle_flash.anm");
            impactModel_ = mc->get("efx_flash.gbs");
            crosshairTex_ = mc->texture("st_xhair_lock");  // original crosshair (red brackets)
            worldRay_ = [w, this](const Vec3& o, const Vec3& d, float maxD, float& t, Vec3& n) {
                float tc = 0;
                Vec3 nc;
                int tag = -1;
                const bool hc = w->collision().raycast(o, d, maxD, tc, nc, &tag);
                lastWorldTag_ = -1;
                // Terrain: march the segment, then bisect the crossing.
                bool ht = false;
                float tt = maxD;
                const float step = 0.5f;
                for (float s = 0; s <= maxD + step * 0.5f; s += step) {
                    const float sc = std::min(s, maxD);
                    const Vec3 p = o + d * sc;
                    float h;
                    if (w->terrain().heightAt(p.x, p.z, h) && p.y < h) {
                        float lo = std::max(0.0f, sc - step), hi = sc;
                        for (int i = 0; i < 8; ++i) {
                            const float mid = 0.5f * (lo + hi);
                            const Vec3 q = o + d * mid;
                            float hq;
                            if (w->terrain().heightAt(q.x, q.z, hq) && q.y < hq) hi = mid; else lo = mid;
                        }
                        tt = hi;
                        ht = true;
                        break;
                    }
                }
                if (!hc && !ht) return false;
                if (hc && (!ht || tc < tt)) { t = tc; n = nc; lastWorldTag_ = tag; return true; }
                t = tt;
                const Vec3 p = o + d * tt;
                float hx0 = p.y, hx1 = p.y, hz0 = p.y, hz1 = p.y;
                w->terrain().heightAt(p.x - 0.5f, p.z, hx0); w->terrain().heightAt(p.x + 0.5f, p.z, hx1);
                w->terrain().heightAt(p.x, p.z - 0.5f, hz0); w->terrain().heightAt(p.x, p.z + 0.5f, hz1);
                n = normalize(Vec3{hx0 - hx1, 1.0f, hz0 - hz1});
                return true;
            };
            
            // Player bolts hit buildings/terrain (-1) or enemies (index).
            weapon_.raycast = [this](const Vec3& o, const Vec3& d, float maxD, float& t, Vec3& n, int& target) {
                float tw = maxD, te = maxD;
                Vec3 nw, ne;
                int ie = -1;
                bool hw = worldRay_ && worldRay_(o, d, maxD, tw, nw);
                bool he = enemies_.raycast(o, d, maxD, te, ne, ie);
                float tc = maxD;
                Vec3 nc;
                int ic = -1;
                const bool hcr = creatures_.raycast(o, d, maxD, tc, nc, ic);
                if (hcr && (!he || tc < te)) {
                    he = true; te = tc; ne = nc; ie = kCreatureTarget + ic;
                }
                static const bool shotTraceC = std::getenv("GVR_SHOT_TRACE") != nullptr;  // (test hook: what each shot meets)
                if (shotTraceC) {
                    float nearest = 1e9f;
                    int ni = -1;
                    for (size_t k = 0; k < creatures_.creatures.size(); ++k)
                        if (creatures_.creatures[k].state != CreatureState::Dead && length(creatures_.creatures[k].pos - o) < nearest) { nearest = length(creatures_.creatures[k].pos - o); ni = int(k); }
                    float off = -1, along = 0;
                    int st = -1;
                    float sc = 0, yy = 0;
                    if (ni >= 0) {
                        const auto& c = creatures_.creatures[size_t(ni)];
                        along = dot(c.pos - o, d);
                        off = length(c.pos - (o + d * along));
                        st = int(c.state); sc = c.scale; yy = c.pos.y;
                    }
                    LOGI("shot trace: from (%.2f %.2f %.2f) dir (%.2f %.2f %.2f): world %d at %.2f, creature %d at %.2f (%d); nearest creature %d %.2f m (state %d, scale %.2f, feet y %.2f, %.2f along, %.2f off the line)",
                         o.x, o.y, o.z, d.x, d.y, d.z, int(hw), hw ? tw : -1.0f, int(hcr), hcr ? tc : -1.0f, ic, ni, nearest, st, sc, yy, along, off);
                }
                // Vimps.
                {
                    float tv = maxD;
                    int iv = -1;
                    if (vimpRay(o, d, maxD, tv, iv) && (!he || tv < te)) { he = true; te = tv; ne = d * -1.0f; ie = kVimpTarget + iv; }
                }
                // Skirmish players.
                {
                    float ts = maxD;
                    int is = -1;
                    if (skirmishRay(o, d, maxD, ts, is) && (!he || ts < te)) { he = true; te = ts; ne = d * -1.0f; ie = is; }
                }
                // Rework: Smarties of a village.
                {
                    float tv = maxD;
                    int iv = -1;
                    if (villagerRay(o, d, maxD, tv, iv) && (!he || tv < te)) { he = true; te = tv; ne = d * -1.0f; ie = kVillagerShot + iv; }
                }
                // Mission-target characters without AI (Offspring, Charger).
                {
                    float tb2 = maxD;
                    int ib2 = -1;
                    if (bossRay(o, d, maxD, tb2, ib2) && (!he || tb2 < te)) { he = true; te = tb2; ne = d * -1.0f; ie = kBossTarget + ib2; }
                }
                // Ripper nests: a sphere around the mound.
                for (size_t k = 0; k < nests_.size(); ++k) {
                    const NestInst& n = nests_[k];
                    if (!n.alive || n.dying >= 0) continue;
                    const float r = std::max(0.6f, n.radius * 0.45f * n.shown);
                    const Vec3 c = n.pos + Vec3{0, r * 0.6f, 0};
                    const Vec3 oc = o - c;
                    const float b = dot(oc, d), cc = dot(oc, oc) - r * r, disc = b * b - cc;
                    if (disc < 0) continue;
                    float tn = -b - std::sqrt(disc);
                    if (tn < 0 && cc <= 0) tn = 0;  // (fired from inside it, point blank: a hit)
                    if (tn < 0 || tn > maxD || (he && tn >= te)) continue;
                    he = true; te = tn; ne = normalize(o + d * tn - c); ie = kNestTarget + int(k);
                }
                // Bodies on the ground can be shot too.
                float tb = maxD;
                int ib = -1;
                if (corpseRay(o, d, maxD, tb, ib) && (!he || tb < te)) { he = true; te = tb; ne = d * -1.0f; ie = ib; }
                // Scenery: a destructible object the world ray hit, or a tree trunk.
                int worldTarget = -1;
                if (hw && lastWorldTag_ >= 0 && size_t(lastWorldTag_) < objectItem_.size() && objectItem_[size_t(lastWorldTag_)] >= 0)
                    worldTarget = kObjectTarget + objectItem_[size_t(lastWorldTag_)];
                float tt = maxD;
                int ti = -1;
                if (treeRay(o, d, std::min(maxD, hw ? tw : maxD), tt, ti) && (!he || tt < te)) {
                    hw = true; tw = tt; nw = d * -1.0f; worldTarget = kObjectTarget + ti;
                }
                if (!hw && !he) return false;
                if (he && (!hw || te < tw)) { t = te; n = ne; target = ie; }
                else { t = tw; n = nw; target = worldTarget; }
                return true;
            };
            weapon_.onHit = [this](int target, const Vec3& p, const Vec3& shotDir) {
                const float hm = hitMul_ * weapon_.hitDamage();  // the gun that fired it (sniper, machine gun) x the hero's power
                static const bool hitTrace = std::getenv("GVR_HIT_TRACE") != nullptr;
                if (hitTrace)
                    LOGI("hit trace: target %d at %.1f m (%.1f %.1f %.1f), damage x%.1f, world object %s", target, length(p - player_.position), p.x, p.y, p.z, hm,
                         lastWorldTag_ >= 0 && assetWorld_ && size_t(lastWorldTag_) < assetWorld_->worldObjects().size() ? assetWorld_->worldObjects()[size_t(lastWorldTag_)].name.c_str() : "-");
                // Rework: an arrow stays where it struck (in the ground, the scenery, a Reaper).
                if (weapon_.hitStyle() == 3 || weapon_.hitStyle() == 4) stickArrow(target >= 0 && target < kCreatureTarget ? target : -1, p, shotDir);
                if (weapon_.hitStyle() == 4) startBlaze(p);  // (a fire arrow: a blaze where it struck)
                if (target >= kVillagerShot) {
                    hitVillager(target - kVillagerShot, 25.0f * hm, p, shotDir);
                    haptics_.push_back({rig_.turnHand(), 0.4f, 0.04f});
                    return;
                }
                if (target >= kObjectTarget) {
                    const int item = target - kObjectTarget;
                    damageObject(item, 25.0f * hm, false, player_.position);
                    const DestructKind k = destruct_.items[size_t(item)].kind;
                    sfx(k == DestructKind::Tree || k == DestructKind::House ? "ex_woods" : "ex_stons", p, 0.45f, 1.1f + 0.2f * frand());
                    smokePuff(k == DestructKind::Tree ? FxTex::Dust : FxTex::Smoke, p, normalize(shotDir * -1.0f) * 0.8f, 0.2f, 0.9f, 0.8f, 0.6f, 0.2f);
                    return;
                }
                // Gunfire carries: Reapers nearby hear it, buried Rippers feel the shot land.
                enemies_.alertNear(p, 20.0f);
                creatures_.disturb(p, 12.0f);
                if (target >= kBossTarget && target < kObjectTarget) {
                    hitBoss(target - kBossTarget, 25.0f * hm, p, shotDir);
                    haptics_.push_back({rig_.turnHand(), 0.4f, 0.04f});
                    return;
                }
                if (target >= kCorpseEnemy) {
                    shootCorpse(target, p, shotDir);
                    haptics_.push_back({rig_.turnHand(), 0.4f, 0.04f});
                    return;
                }
                if (target < 0) {
                    if (shotIntoWater(p, shotDir)) return;  // a splash where it went in
                    addSparks(p, shotDir * -1.0f, 5 + int(3.0f * frand()));
                    sfx(frand() < 0.5f ? "bi_dirt1" : (frand() < 0.5f ? "bi_hit1" : "bi_hit2"), p, 0.45f, 0.9f + 0.2f * frand());
                    return;
                }
                if (target >= kVimpTarget && target < kNestTarget) {
                    hitVimp(target - kVimpTarget, 25.0f * hm, p, shotDir);
                    haptics_.push_back({rig_.turnHand(), 0.4f, 0.04f});
                    return;
                }
                if (target >= kNestTarget) {
                    damageNest(target - kNestTarget, 25.0f * hm);
                    haptics_.push_back({rig_.turnHand(), 0.5f, 0.05f});
                    return;
                }
                if (target >= kBotTarget && target < kBotTarget + 1000) {
                    skirmishHit(target - kBotTarget, 25.0f * hm, p, shotDir);
                    return;
                }
                if (target >= kCreatureTarget) {
                    std::vector<CreatureEvent> ev;
                    const int ci = target - kCreatureTarget;
                    const float scale = creatures_.creatures[size_t(ci)].scale;
                    const int slot = creatures_.creatures[size_t(ci)].slot;
                    // A Ripper's head (round its mouth) is its weak spot.
                    Vec3 mouth;
                    const bool weak = creatures_.mouth && creatures_.mouth(ci, mouth) && length(mouth - p) < 0.55f * scale;
                    const bool died = creatures_.damage(ci, (weak ? 45.0f : 25.0f) * hm, shotDir, &ev);
                    if (died && slot >= 0 && size_t(slot) < chars_.size() && chars_[size_t(slot)].objective >= 0)
                        completeObjective(chars_[size_t(slot)].objective);
                    bleed(p, shotDir, died, scale);
                    if (died && !creatures_.creatures[size_t(ci)].exploded) blood_.pool(creatures_.creatures[size_t(ci)].pos, 0.9f * scale);
                    for (const auto& e : ev)
                        if (e.kind == CreatureEvent::Die && e.burst) {
                            spawnGore(e.pos, e.dir, scale);
                            blasts_.push_back({e.pos + Vec3{0, 0.5f * scale, 0}, 3, 0.0f});
                        }
                    haptics_.push_back({rig_.turnHand(), died ? 0.8f : 0.5f, died ? 0.12f : 0.05f});
                    sfx("ex_bdys1", p, 0.35f, 1.2f + 0.2f * frand());
                    for (const auto& e : ev) creatureSound(e);
                    return;
                }
                // Headshots (hit above the shoulders) hurt a Reaper two and a half times as much.
                // (a shot at the gun he holds up to his face is a gun hit, not a head shot)
                Vec3 gunAt{1e9f, 1e9f, 1e9f};
                reaperGunCentre(enemies_.enemies[size_t(target)], gunAt);
                // (the shot stops at his capsule; the gun he holds out is often in front of it, so the shot
                // whose line passed through the gun, near where it stopped, hit the gun)
                const Vec3 sd = length(shotDir) > 1e-4f ? normalize(shotDir) : Vec3{0, 0, -1};
                const Vec3 toGun = gunAt - p;
                const float along = dot(toGun, sd);
                const float offLine = length(toGun - sd * along);
                const bool atGun = (length(toGun) < 0.45f || (offLine < 0.22f && std::fabs(along) < 0.9f)) &&
                                   !enemies_.enemies[size_t(target)].disarmed;
                const bool head = !atGun && p.y - enemies_.enemies[size_t(target)].pos.y > 1.6f * enemies_.bodyHeight(enemies_.enemies[size_t(target)]) / std::max(0.5f, enemies_.params.height);
                if (head) sfx("bi_hit2", p, 0.8f, 1.4f);
                // Rework: the hit shoves him (a killing shot sends the body back - it slides on), more for
                // heavier guns and head hits.
                if (const float hl = std::sqrt(shotDir.x * shotDir.x + shotDir.z * shotDir.z); hl > 1e-3f)
                    enemies_.push(target, Vec3{shotDir.x / hl, 0, shotDir.z / hl} * ((head ? 3.0f : 1.8f) * std::min(3.0f, std::max(0.6f, hm))));
                enemies_.enemies[size_t(target)].hitHeight = (p.y - enemies_.enemies[size_t(target)].pos.y) / std::max(0.5f, enemies_.bodyHeight(enemies_.enemies[size_t(target)]));
                // Rework: where he's hit counts. The gun (or the hand on it) shot: it flies out of his hand
                // (often) and he comes in with his fists; a leg: he limps for a while.
                Enemy& he = enemies_.enemies[size_t(target)];
                float limbD = 0;
                const std::string limb = he.state != EnemyState::Dead && !head ? nearestLimb(he, p, limbD) : std::string();
                // (shots stop at his capsule, a little in front of the gun: 0.45 m)
                const bool gunHit = he.weaponOn && !he.disarmed && (atGun || ((limb == "r_arm" || limb == "arm r") && limbD < 0.3f));
                const bool legHit = !gunHit && (limb == "l_thigh" || limb == "r_thigh" || limb == "leg l" || limb == "leg r") && he.hitHeight < 0.45f;
                if (std::getenv("GVR_HIT_TRACE")) LOGI("hit trace: Reaper %u (state %d, slot %d, head %d, gun %d) at height %.2f, line %.2f along %.2f, %.2f m from his gun, nearest limb %s %.2f m", he.uid, int(he.state), he.slot, int(head), int(atGun), he.hitHeight, offLine, along, length(p - gunAt), limb.c_str(), limbD);
                const bool killed = enemies_.damage(target, (head ? 62.5f : (atGun ? 12.0f : 25.0f)) * hm);  // (the gun takes most of it)
                if (!killed && gunHit && (atGun ? frand() < 0.7f : frand() < 0.4f)) {
                    Vec3 away = shotDir + Vec3{0, 0.6f, 0};
                    dropReaperGun(he, away * 4.5f);
                    he.disarmed = true;
                    sfx("sr_swht1", gunAt, 0.9f, 1.6f + 0.2f * frand());  // (a metal ping, as the parry)
                    LOGI("Reaper %u: his gun shot out of his hand", he.uid);
                }
                if (!killed && legHit) {
                    he.limpT = 5.0f;
                    static const bool trace = std::getenv("GVR_HIT_TRACE") != nullptr;
                    if (trace) LOGI("Reaper %u: shot in the leg, limping", he.uid);
                }
                if (killed && enemies_.enemies[size_t(target)].objective >= 0)
                    completeObjective(enemies_.enemies[size_t(target)].objective);
                bleed(p, shotDir, killed, 1.0f);
                if (killed) {
                    blood_.pool(enemies_.enemies[size_t(target)].pos, 1.3f);
                    enemies_.alertNear(enemies_.enemies[size_t(target)].pos, 25.0f);  // a comrade falls
                    // Rework: a sniper's head shot takes the head off. (The shotgun's shells burst: the
                    // blast path in game_app_ragdoll.cpp tears off the limb nearest the burst.)
                    Enemy& ke = enemies_.enemies[size_t(target)];
                    if (hero_ == Hero::Mecc && gunValid_ && !ke.gibbed && head && gunOfSel(weaponSel_) == kGunSniper) dismember(ke, p, shotDir);
                }
                haptics_.push_back({rig_.turnHand(), killed ? 0.8f : 0.5f, killed ? 0.12f : 0.05f});
                sfx("ex_bdys1", p, 0.35f, 1.1f + 0.2f * frand());
                static const char* hits[] = {"rpman_hit1", "rpman_hit2", "rpman_hit3", "rpman_hit4"};
                static const char* dies[] = {"rpman_die1", "rpman_die2", "rpman_die3", "rpman_die4", "rpman_die5"};
                if (killed) sfx(dies[int(frand() * 4.99f)], p, 1.0f);
                else if (frand() < 0.6f) sfx(hits[int(frand() * 3.99f)], p, 0.9f);
            };
            // Enemy bolts hit buildings/terrain (-1) or the player (-2: vertical capsule).
            enemyFire_.projectileSpeed = 32.0f;  // slow enough to see and dodge
            enemyFire_.raycast = [this](const Vec3& o, const Vec3& d, float maxD, float& t, Vec3& n, int& target) {
                float tw = maxD, tp = maxD;
                Vec3 nw;
                const bool hw = worldRay_ && worldRay_(o, d, maxD, tw, nw);
                const float hs = heroScale_;
                // The capsule's top follows your real head (rework): duck behind cover and the
                // bolts pass over you.
                const float top = playerHitTop();
                const Vec3 a = player_.position + Vec3{0, 0.35f * hs, 0}, b = player_.position + Vec3{0, top, 0};
                const bool hp = playMode_ && rayCapsule(o, d, maxD, a, b, player_.radius, tp);
                if (!hw && !hp) return false;
                if (hp && (!hw || tp < tw)) { t = tp; n = d * -1.0f; target = -2; }
                else { t = tw; n = nw; target = -1; }
                return true;
            };
            enemyFire_.onHit = [this](int target, const Vec3& p, const Vec3& dir) {
                if (target == -2) { hitFrom_ = p - dir * 10.0f; hitDirTimer_ = 1.2f; damagePlayer(5.0f, DeathCause::Shot); }
                else if (!shotIntoWater(p, dir)) {
                    sfx(frand() < 0.5f ? "bi_hit1" : "bi_dirt1", p, 0.35f, 0.9f + 0.2f * frand());
                    if (length(p - player_.position) < 40.0f) addSparks(p, dir * -1.0f, 3);  // (near you: the misses you see)
                }
            };
            // NPCs float in deep water (the sea surface is at 0): they can't walk on the sea floor.
            enemies_.groundHeight = [w](float x, float z) {
                float y;
                return std::max(w->terrain().heightAt(x, z, y) ? y : -20.0f, -1.1f);
            };
            enemies_.collision = &w->collision();
            // Rework: cover for Reapers - beside the far side of a solid object of rock / tree / hut size
            // standing on the ground, as seen from the player.
            enemies_.findCover = [w](const Vec3& near, const Vec3& player, Vec3& spot, bool& tree) {
                float best = 12.0f;
                bool found = false;
                const ObjectRegistry& reg = w->registry();
                for (const auto& o : w->worldObjects()) {
                    if (o.collision < 0 || o.hidden) continue;
                    // A tree (class 60): its trunk at the placement point, whatever the fronds' reach
                    // (rework: they hide standing behind it). Else a rock, a wall or a hut of fitting size.
                    const ObjectType* ot = reg.type(o.type);
                    const bool isTree = ot && ot->objectClass == 60;
                    if (!isTree && (o.radius < 0.7f || o.radius > 6.0f)) continue;
                    const Vec3 c = isTree ? Vec3{o.placement.m[12], 0, o.placement.m[14]} : Vec3{o.centre.x, 0, o.centre.z};
                    const float dn = length(Vec3{c.x - near.x, 0, c.z - near.z});
                    if (dn > best) continue;
                    float gy;
                    if (!w->terrain().heightAt(c.x, c.z, gy)) continue;
                    if (!isTree && o.centre.y - gy > 1.5f * o.radius) continue;  // (in the air: a bridge, a roof)
                    Vec3 away{c.x - player.x, 0, c.z - player.z};
                    const float da = length(away);
                    if (da < 6.0f || da > 40.0f) continue;
                    away = away * (1.0f / da);
                    const Vec3 perp{-away.z, 0, away.x};
                    const float sideSign = dot(Vec3{near.x - c.x, 0, near.z - c.z}, perp) >= 0 ? 1.0f : -1.0f;
                    Vec3 p = isTree ? c + away * 0.75f : c + away * (0.6f * o.radius + 0.8f) + perp * (sideSign * 0.55f * o.radius);
                    if (!w->terrain().heightAt(p.x, p.z, p.y)) continue;
                    if (p.y < -0.5f) continue;  // (not in the sea)
                    best = dn;
                    spot = p;
                    tree = isTree;
                    found = true;
                }
                return found;
            };
            // Reaper grunt model pool with the original clips (order = EnemyClip).
            const char* eclips[] = {"rpman_stand_scan.anm", "rpman_walk_fire.anm", "rpman_stand_fire.anm",
                                    "rpman_hit_med.anm", "rpman_hit_med2.anm", "rpman_hit_med_die.anm",
                                    "rpman_hit_spin_die.anm", "rpman_hit_gut_die.anm", "rpman_bzka_run.anm",
                                    "rpman_bzka_walk_scan.anm", "rpman_standing_alert_scan.anm", "rpman_hit_blowup.anm",
                                    "rpman_hit_fall_cycle.anm", "rpman_hit_fall_land.anm", "rpman_poke_bush.anm",
                                    // rework: kneeling marksman, punch, head-shot and leg-shot deaths, grenade throw
                                    "rpman_snipe_kneel_fire.anm", "rpman_punch.anm", "rpman_hit_eye_die.anm",
                                    "rpman_hit_to_knees_die.anm", "rpman_grenade.anm", "rpman_Hover.anm", "rpman_crouch_fire.anm", "rpman_hit_big.anm", "rpman_hit_big_getup.anm", "rpman_bzka_kneel_fire.anm", "rpman_crouch_breathe.anm", "rpman_rock_into.anm", "rpman_rock_breathe.anm", "rpman_behind_tree_hide.anm", "rpman_behind_tree_breathe.anm", "rpman_behind_tree_peek.anm"};
            int poolSize = 6;
            {
                int missionReapers = 0;
                for (const auto& ch : w->characters()) missionReapers += ch.objectClass == 51;
                // (rework: barracks send out more of them)
                int barracks = 0;
                for (const auto& o : w->worldObjects())
                    barracks += o.name == "barracks" || o.name == "barracks_story2" || o.name == "bw_minibarracks" || o.name == "guard_army_barracks";
                poolSize = std::min(16, std::max(6, missionReapers) + 3 * std::min(barracks, 2));
            }
            for (int k = 0; k < poolSize; ++k) {
                auto em = std::make_unique<SkinnedModel>();
                if (!em->load(w->fs(), *mc, renderer_, "rpman_l0", eclips[0])) break;
                for (int c = 1; c < int(EnemyClip::Count); ++c) em->addAnimation(w->fs(), eclips[c]);
                if (k == 0 && std::getenv("GVR_BONES")) em->setHiddenBones({});  // (debug: lists the Reaper's bones)
                if (k == 0) {
                    for (int c = 0; c < int(EnemyClip::Count); ++c) enemies_.clipDuration[c] = std::max(0.1f, em->animDuration(c));
                    em->pose(renderer_, 0.0f);
                    Vec3 mn, mx;
                    em->posedBounds(mn, mx);
                    enemyFootOffset_ = -mn.y;  // measured: 1.10 m for rpman_stand_scan
                    LOGI("Reaper foot offset %.2f m", enemyFootOffset_);
                }
                // Rework: a dead body's pose comes from its ragdoll (game_app_ragdoll.cpp).
                em->setPoseModifier([this](const AnmAnimation& clip, const std::vector<std::vector<int>>&, std::vector<Mat4>& world) {
                    if (!posingEnemy_) return;
                    Ragdoll* rd = ragdollOf(*posingEnemy_);
                    if (!rd) return;
                    const Mat4 tw = ragdollToWorld(*posingEnemy_);
                    if (rd->pending) {
                        std::vector<int> parent(clip.nodes.size(), -1);
                        for (size_t n = 0; n < clip.nodes.size(); ++n) parent[n] = clip.nodes[n].parent;
                        rd->start(world, parent, tw, rd->startVel, rd->startSpin, 0.4f);
                        rd->pending = false;
                    }
                    rd->apply(world, tw);
                    if (rd->asleep) rd->posedAsleep = true;
                });
                enemyPool_.push_back(std::move(em));
            }
            ragdolls_.assign(enemyPool_.size(), Ragdoll{});
            slotUsed_.assign(enemyPool_.size(), 0);
            slotLift_.assign(enemyPool_.size(), enemyFootOffset_);
            const uint8_t white[4] = {255, 255, 255, 255};
            whiteTex_ = renderer_.uploadTexture(1, 1, white, false, false);
            LOGI("Enemies: %zu Reaper model slots", enemyPool_.size());
            initCharacters(w, mc);
            // (before the mission: a multiplayer map's computer players copy the player's ground and sea)
            player_.collision = &w->collision();
            // Off the terrain heightfield is open sea; the sea (surface at 0) holds the player up.
            player_.groundHeight = [w](float x, float z) {
                float y;
                return w->terrain().heightAt(x, z, y) ? y : -20.0f;
            };
            player_.seaLevel = 0.0f;
            initMission();
            cutscene_ = std::make_unique<CutscenePlayer>(w->fs(), *mc, renderer_, w->registry());
            cutscene_->groundHeight = [w](float x, float y, float z) {
                float g;
                float ground = w->terrain().heightAt(x, z, g) ? g : -1e9f;
                // A walkway, rock or roof under the feet (at most a little above them) counts too.
                float top;
                if (w->collision().groundAt(x, z, y + 0.4f, top)) ground = std::max(ground, top);
                return ground;
            };
            text_.init(w->fs(), renderer_);
            {
                std::vector<uint8_t> tb;
                auto gt = std::make_unique<GameText>();
                if (w->fs().read("GTextEnglish.bin", tb) && gt->parse(tb.data(), tb.size())) {
                    LOGI("Game text: %zu entries", gt->size());
                    gameText_ = std::move(gt);
                }
            }
        } else {
            LOGW("Player model unavailable; free camera only");
        }
    }
    if (title_) enterTitle();
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
    // Kabuto: the camera distances grow with him (heroScale_), the tracking space with the
    // giant's eye height (rig_.comfort.worldScale, see updateHeroScale).
    const float hs = heroScale_, ws = rig_.comfort.worldScale;
    const float kBack = 3.6f * hs, kUp = 1.1f * hs, kSmooth = 0.12f;
    const Quat q = Quat::fromAxisAngle({0, 1, 0}, camYaw_);
    Vec3 pos = player_.position;
    if (firstPerson_) {
        // The player position IS the head (horizontally): the tracking origin is placed so the
        // tracked head lands on it; the floor is at the character's feet.
        if (in && in->headValid) pos = pos - q.rotate(Vec3{in->head.position.x, 0, in->head.position.z} * ws);
        pos.y += seatLift() * ws;  // (seated: the real floor maps above the feet)
        rig_.worldFromTracking = Pose{q, pos};
        camFloor_ = player_.position.y;
        return;
    }
    float floorY = player_.position.y + (firstPerson_ ? 0.0f : kUp);
    if (!firstPerson_) {
        pos += q.rotate(Vec3{0, 0, kBack});
        if (player_.groundHeight) floorY = std::max(floorY, player_.groundHeight(pos.x, pos.z) + 0.3f * hs);
    }
    if (snap || dt <= 0) camFloor_ = floorY;
    else camFloor_ += (floorY - camFloor_) * (1.0f - std::exp(-dt / kSmooth));
    pos.y = (firstPerson_ ? player_.position.y : camFloor_) + seatLift() * ws;  // (seated: the real floor maps above the feet)
    // Camera collision: if a cliff or building is between the character and the viewer's eyes,
    // pull the camera in front of it so the character stays visible.
    const float eyeH = ((in && in->headValid) ? in->head.position.y : 1.6f) * ws;  // (pos already has the seated lift)
    const Vec3 chest = player_.position + Vec3{0, 1.4f * hs, 0};
    const Vec3 eye = pos + Vec3{0, eyeH, 0};
    const Vec3 toEye = eye - chest;
    const float dist = length(toEye);
    float t;
    Vec3 n;
    if (worldRay_ && dist > 0.5f && worldRay_(chest, toEye * (1.0f / dist), dist, t, n)) {
        const Vec3 newEye = chest + toEye * (std::max(0.3f * hs, t - 0.4f * hs) / dist);
        pos = newEye - Vec3{0, eyeH, 0};
    }
    rig_.worldFromTracking = Pose{q, pos};
}

void GameApp::updatePlayMode(float dt, const InputFrame& in) {
    GVR_PROF("playmode (all)");
    const int mh = rig_.moveHand(), th = rig_.turnHand();
    if (viewEdge_.pressed(in.hand[th].thumbstickClick)) {
        firstPerson_ = !firstPerson_;
        LOGI("Camera: %s person", firstPerson_ ? "first" : "third");
    }
    const Quat qOld = Quat::fromAxisAngle({0, 1, 0}, camYaw_);
    if (!wheelOpen_ && stickFwdT_ <= 0) camYaw_ += rig_.turnInput(dt, in);  // (the stick held forward for the wheel: no turning)
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
            player_.position += qOld.rotate(Vec3{in.head.position.x - headPrev_.x, 0, in.head.position.z - headPrev_.z} * rig_.comfort.worldScale);
        headPrev_ = in.head.position;
        haveHeadPrev_ = true;
        // Eye height estimate for the body scale: smoothed, ignoring implausible values, so the
        // brief lift while putting the headset on does not inflate the body (old running max did).
        const float hy = in.head.position.y + seatLift();
        if (hy > 1.0f && hy < 2.05f) {
            if (!eyeCalibrated_) { userEye_ = hy; eyeCalibrated_ = true; }
            else if (dt > 0) userEye_ += (hy - userEye_) * (1.0f - std::exp(-dt / 2.5f));
        }
    }
    // The jetpack on his back only once he has it.
    if (playerSpare_ && playerHasJet_ != player_.jetpackAvailable) {
        std::swap(playerModel_, playerSpare_);
        playerHasJet_ = !playerHasJet_;
        LOGI("Player model: %s jetpack", playerHasJet_ ? "with" : "without");
    }
    updateHeroScale();
    { GVR_PROF("follow head"); followHead(dt, in); }
    briefHeld_ = in.hand[mh].thumbstickClick;  // hold the move stick down: the mission briefing
    renderer_.environment().pointLightColor = Vec3{0, 0, 0};  // the jetpack's light (updateJetFx) is off unless lit
    if (dying_) {
        updateDeath(dt, in);
        updateCombat(dt);
        { GVR_PROF("audio"); updateAudio(dt); }
        updateFlashes(dt);
        return;
    }
    { GVR_PROF("jetski world"); updateJetskiWorld(dt, in); }
    if (riding_) {  // on the Reaperski: its own handling (game_app_jetski.cpp)
        swordValid_ = gunValid_ = false;
        repairOn_ = false;
        endCloak("she rides");  // (updateBush is skipped here: nothing else clears her hiding)
        enemies_.playerHidden = enemies_.playerMoving = false;
        updateRiding(dt, in);
        { GVR_PROF("feel"); updateFeel(dt, in); }
        updateCombat(dt);
        { GVR_PROF("audio"); updateAudio(dt); }
        updateFlashes(dt);
        return;
    }
    PlayerInput pi;
    pi.move = in.hand[mh].thumbstick;
    pi.viewYaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : camYaw_;
    pi.jet = in.hand[th].primary;              // A
    pi.sprint = in.hand[mh].squeeze > 0.5f && !twoHand_;    // move-hand grip (not while it steadies a gun)
    if (hero_ == Hero::Kabuto && kbGrabHand_ == mh) pi.sprint = false;  // that grip holds something
    if (heldBody_.hand == mh || (heldVillager_ != 0 && heldVillagerHand_ == mh) || stoneInHand(mh)) pi.sprint = false;  // (a body, a Smartie, a stone)
    if (!reachPath_.empty()) reachWalkInput(pi.move, pi.viewYaw, pi.jet, dt);  // test hook GVR_REACH_WALK
    kabutoFeedInput(pi.move, pi.viewYaw, dt);                                   // test hook GVR_KABUTO_FEED
    updateTeleport(dt, pi, in);                                                 // Options -> MOVEMENT: TELEPORT
    updateWristPanel(dt);
    // Rework: Kabuto leaps when you jump - a quick rise of your real head from standing (first person)
    // springs him up half as high again as the button's jump.
    kbLeapCool_ = std::max(0.0f, kbLeapCool_ - dt);
    if ((hero_ == Hero::Kabuto || hero_ == Hero::Delphi) && firstPerson_ && in.headValid && dt > 0) {
        const float y = in.head.position.y + seatLift();  // (as userEye_: seated, the lifted height)
        const float vy = kbHeadValid_ ? (y - kbHeadPrevY_) / dt : 0.0f;
        kbHeadVelY_ = kbHeadVelY_ * 0.5f + vy * 0.5f;
        kbHeadPrevY_ = y;
        kbHeadValid_ = true;
        if (kbHeadVelY_ > 1.3f && y > userEye_ - 0.05f && player_.onGround && kbLeapCool_ <= 0 && !pi.jet) {
            pi.jet = true;
            pi.jumpBoost = 1.25f;  // (x1.25 speed: ~1.5 x the height)
            kbLeapCool_ = 1.2f;
            haptics_.push_back({kLeft, 0.8f, 0.15f});
            haptics_.push_back({kRight, 0.8f, 0.15f});
            LOGI("%s leaps (your head rose at %.1f m/s)", hero_ == Hero::Kabuto ? "Kabuto" : "Delphi", kbHeadVelY_);
        }
        // ...and a quick drop of your head (a crouch slammed down) is a ground pound round his feet.
        if (hero_ == Hero::Kabuto && kbHeadVelY_ < -1.6f && y < userEye_ - 0.15f && player_.onGround && kbLeapCool_ <= 0) {
            kbLeapCool_ = 1.2f;
            float gy = player_.position.y;
            if (player_.groundHeight) gy = player_.groundHeight(player_.position.x, player_.position.z);
            kabutoQuake(Vec3{player_.position.x, gy, player_.position.z}, 0.45f * player_.height, 90.0f);
            sfx2D("ka_stomp", 1.0f, 0.9f);
            haptics_.push_back({kLeft, 1.0f, 0.2f});
            haptics_.push_back({kRight, 1.0f, 0.2f});
            LOGI("Kabuto slams the ground (your head dropped at %.1f m/s)", -kbHeadVelY_);
        }
    } else {
        kbHeadValid_ = false;
    }
    if (hero_ == Hero::Delphi && delphiBow() && firstPerson_ && (bowDrawing_ || (in.hand[mh].gripPoseValid &&
        length(rig_.toWorld(in.hand[mh].gripPose).position - bowNockRest_) < 0.3f)))
        pi.sprint = false;  // that grip is drawing her bowstring, not running
    if (!firstPerson_ && hero_ != Hero::Kabuto) {
        { GVR_PROF("tp aim"); updateThirdPersonAim(in, dt); }
        pi.aiming = tpAiming_;
        const Vec3 d = aimTarget_ - player_.position;
        pi.aimYaw = std::atan2(-d.x, -d.z);
    } else {
        tpAiming_ = false;
    }
    updateClimb(dt, in);
    if (player_.climbing) { pi.move = Vec2{0, 0}; pi.sprint = false; pi.jet = false; }
    pi.stroke = updateSwimStrokes(dt, in);
    updateDive(dt, in, pi);
    updateBush(dt, pi);
    if (cloakT_ > 0) {  // (Delphi's cloak: unseen as in the bush; only a quick dash catches an eye nearby)
        enemies_.playerHidden = true;
        enemies_.playerMoving = player_.horizontalSpeed() > 4.0f;
    }
    { GVR_PROF("player"); player_.update(dt, pi); }
    if (const char* t = std::getenv("GVR_PLAYER_TURN")) player_.yaw = camYaw_ + std::atof(t) * kPi / 180.0f;  // test hook: side views
    // Hard landings hurt (the original: damage and a 2 s stun; a rocket under him prevents it).
    const float impact = player_.takeImpact();
    if (impact > player_.params.stunSpeed && !player_.swimming()) {
        damagePlayer((impact - player_.params.stunSpeed) * 6.0f + 5.0f, DeathCause::Fall);
        sfx2D("me_hdfl1", 1.0f, 0.9f);
        LOGI("Hard landing at %.1f m/s", impact);
        timmySay("smchride4", true);  // "I feel sick."

    }
    if (player_.takeOverheat()) {
        sfx2D("me_jpkno", 0.9f);
        LOGI("Jetpack burnt dry: overheated");
    }
    { GVR_PROF("pickups"); updatePickups(dt, in); }
    updateShop(dt, in);
    updateBase(dt, in);
    updatePopup(dt, in);
    updateGrenades(dt, in);
    updateMines(dt, in);
    { GVR_PROF("map"); updateMap(dt, in); }
    updateFlares(dt, in);
    { GVR_PROF("camera"); placeCamera(dt, false, &in); }
    if (!playerModel_) return;
    if (firstPerson_) {
        { GVR_PROF("fp body"); poseFirstPersonBody(in); }
        if (syringeOut_) armTargets_[rig_.moveHand()].squeeze = 1.0f;  // the hand closes on the syringe
    } else {
        bodyXf_ = Mat4::translation(player_.position + Vec3{0, heroLift_, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, player_.yaw));
        if (hero_ == Hero::Kabuto && kbGrowShown_ < 0.999f) bodyXf_ = bodyXf_ * Mat4::scale(Vec3{1, 1, 1} * kbGrowShown_);
        armTargets_[0].valid = armTargets_[1].valid = false;
        // Aiming: the gun arm reaches toward the crosshair (model space, game units).
        Mat4 sh;
        const int gh = rig_.turnHand();
        if (tpAiming_ && playerModel_->nodeWorld(gh == kRight ? "arm r" : "arm l", sh)) {
            const Quat inv = Quat::fromAxisAngle({0, 1, 0}, player_.yaw).conjugate();
            const Vec3 S{sh.m[12], sh.m[13], sh.m[14]};
            const Vec3 sw = player_.position + Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(giantsToRuntime(S.x, S.y, S.z));
            const Vec3 l = inv.rotate(normalize(aimTarget_ - sw));
            const Vec3 dm = normalize(Vec3{-l.x, l.z, l.y});
            ArmPose& a = armTargets_[gh];
            a.valid = true;
            a.grip = S + dm * 2.6f;  // nearly straight arm toward the target
            a.forward = dm;
            a.up = Vec3{0, 0, 1};
            a.squeeze = 1.0f;
            a.trigger = 1.0f;
        }
    }
    playerModel_->setHideEnabled(firstPerson_);
    int bushClip = 0;
    float bushT = 0;
    const auto poseT0 = std::chrono::steady_clock::now();
    if (!firstPerson_ && bushBodyClip(bushClip, bushT)) {
        // Crouched in the bush pack (third person; in first person the body stays as it is).
        const float d = playerModel_->animDuration(bushClip);
        const bool once = bushClip == bushClip_[0] || bushClip == bushClip_[3];
        const float tt = once ? std::min(bushT, d * 0.999f) : std::fmod(bushT, std::max(0.1f, d));
        playerModel_->poseBlend(renderer_, bushClip, tt, bushClip, tt, 0.0f);
    } else {
        playerModel_->poseBlend(renderer_, clip_[int(player_.prevAnim())], player_.prevAnimTime(),
                                clip_[int(player_.anim())], player_.animTime(), player_.fadeWeight());
    }
    if (profEnabled()) profAdd("pose player", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - poseT0).count());
    eyeNow_ = playerModel_->hiddenCentroid() + eyeFromHead_;
    if (in.headValid) headWorldRot_ = rig_.toWorld(in.head).orientation;
    { GVR_PROF("timmy"); updateTimmy(); }
    updateSmartieLines(dt);
    { GVR_PROF("jetfx"); updateJetFx(dt); }
    if (hero_ == Hero::Delphi) updateDelphi(dt, in);
    else if (hero_ == Hero::Kabuto) { updateKabuto(dt, in); updateThrows(dt, in); updateKabutoCarry(dt, in); updateKabutoPowers(dt, in); updateGlass(dt, in); }
    else { GVR_PROF("weapon"); updateHolsters(dt, in); updateWeapon(dt, in); updateMelee(dt, in); }
    { GVR_PROF("feel"); updateFeel(dt, in); }
    { GVR_PROF("rescue"); updateRescue(dt, in); }
    updateReaperGrenades(dt * enemyTimeScale());
    updateAmbush(dt);
    updateMotes(dt);
    updateGrass();
    for (auto& c : chars_)
        if (c.rider && playMode_) {  // on the back: behind the shoulders, facing the way the hero does
            const Vec3 back{std::sin(player_.yaw), 0, std::cos(player_.yaw)};
            c.pos = player_.position + back * (0.35f * heroScale_) + Vec3{0, 0.3f * player_.height, 0};
            c.yaw = player_.yaw;
        }
    {  // test hook: walkability of the "reach" objectives (game_app_reach.cpp), once, a second into play
        static const bool reach = std::getenv("GVR_REACH_CHECK") != nullptr;
        static float reachT = 0;
        if (reach && playMode_ && reachT >= 0 && (reachT += dt) > 1.0f) { reachT = -1; checkReachObjectives(); }
    }
    updateCreatureFlashes(dt);
    updateFireballs(dt);
    updateBlazes(dt);
    updateNearMisses(dt);
    updateHandTouch(dt);
    updateWaveGesture(dt);
    updateCombat(dt);
    { GVR_PROF("audio"); updateAudio(dt); }
        updateFlashes(dt);
}

void GameApp::updateRescue(float dt, const InputFrame& in) {
    if (rescue_.smarties.empty()) return;
    Vec3 hands[2];
    float grip[2];
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        if (firstPerson_ && c.gripPoseValid) {
            hands[h] = rig_.toWorld(c.gripPose).position;
        } else {
            // Third person: the character's arms carry it, in front of the chest.
            const Quat r = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
            hands[h] = player_.position + r.rotate(Vec3{h == kLeft ? -0.25f : 0.25f, 1.3f, -0.7f});
        }
        grip[h] = c.squeeze;
    }
    // Test hook (headless): GVR_AUTO_RESCUE=1 makes the right hand fetch each Smartie in turn and
    // set it down over the pad (exercises grabbing, carrying, saving and mission completion).
    static const bool autoRescue = std::getenv("GVR_AUTO_RESCUE") != nullptr;
    if (autoRescue && rescue_.hasPad) {
        static int phase = 0;
        const Vec3 padTop = (rescue_.padMin + rescue_.padMax) * 0.5f + Vec3{0, 1.0f, 0};
        if (!rescue_.holding(kRight)) {
            phase = 0;
            int pick = -1;
            for (size_t i = 0; i < rescue_.smarties.size() && pick < 0; ++i)
                if (rescue_.smarties[i].state == SmartieState::Idle || rescue_.smarties[i].state == SmartieState::Hanging) pick = int(i);
            if (pick >= 0) {
                const RescueSmartie& sm = rescue_.smarties[size_t(pick)];
                hands[kRight] = sm.pos + Vec3{0, sm.height - 0.2f, 0};
                autoGrip_ = !autoGrip_;  // press on alternate frames (edge)
                grip[kRight] = autoGrip_ ? 1.0f : 0.0f;
            }
        } else {
            hands[kRight] = padTop;
            grip[kRight] = phase++ < 2 ? 1.0f : 0.0f;  // carry over, hold still, let go
        }
    }
    // Third person: let the reach test use the character, not the controllers.
    rescue_.reach = firstPerson_ ? 0.9f : 1.6f;
    rescueEvents_.clear();
    rescue_.update(dt, hands, grip, player_.position, rescueEvents_);
    for (const auto& e : rescueEvents_) {
        const RescueSmartie& s = rescue_.smarties[size_t(e.index)];
        switch (e.kind) {
            // Original sounds: me_smton / me_smtof / me_smtdp (the Meccaryn picking up, letting go
            // of and dropping off a Smartie), the Smarties' gsmNN_1 / _2 voice pairs (used as a
            // pick-up line and a thank-you; their exact use is ours), se_splsh, pi_swrm (piranhas).
            case SmartieEvent::Grabbed: {
                haptics_.push_back({e.hand, 0.5f, 0.06f});
                sfx("me_smton", e.pos, 0.8f);
                LOGI("Smartie %d picked up (%s hand)", e.index, e.hand == kLeft ? "left" : "right");
                static const int lines[] = {1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15};
                rescueLine_ = lines[int(frand() * 13.99f)];
                char n[16];
                std::snprintf(n, sizeof n, "gsm%02d_1", rescueLine_);
                sfx(n, e.pos, 1.0f);
                std::snprintf(n, sizeof n, "smchpickup%d", 1 + int(frand() * 3.99f));  // "Sure, I'll work for you."
                smartieSay(smartieName(uint32_t(e.index) + 1000u), n);
                break;
            }
            case SmartieEvent::Released:
                sfx("me_smtof", e.pos, 0.7f);
                break;
            case SmartieEvent::Saved: {
                sfx("me_smtdp", e.pos, 0.9f);
                char n[16];
                std::snprintf(n, sizeof n, "gsm%02d_2", rescueLine_ ? rescueLine_ : 1);
                sfx(n, e.pos, 1.0f);
                std::snprintf(n, sizeof n, "smchdropoff%d", 1 + int(frand() * 3.99f));
                smartieSay(smartieName(uint32_t(e.index) + 1000u), n);
                haptics_.push_back({kLeft, 0.3f, 0.05f});
                haptics_.push_back({kRight, 0.3f, 0.05f});
                completeObjective(s.objective);
                LOGI("Smartie %d saved (%zu/%zu)", e.index, rescue_.savedCount(), rescue_.smarties.size());
                // 1b's briefing (MO_1b_1_catchsmarties) asks for four Smarties of the eight on the
                // pillars, as does the original's walkthrough: four saved completes the rest.
                if (rescue_.savedCount() >= size_t(kRescueNeeded))
                    for (const auto& o : rescue_.smarties) completeObjective(o.objective);
                break;
            }
            case SmartieEvent::Slipping: {
                char n[16];
                std::snprintf(n, sizeof n, "gsm%02d_1", 1 + int(frand() * 5.99f));
                sfx(n, e.pos, 1.0f, 1.1f);
                LOGI("Smartie %d is slipping", e.index);
                break;
            }
            case SmartieEvent::LetGo:
                LOGI("Smartie %d lost its grip", e.index);
                break;
            case SmartieEvent::Died:
                LOGI("Smartie %d was eaten by piranhas", e.index);
                break;
            case SmartieEvent::Returned:
                LOGI("Smartie %d is back", e.index);
                break;
            case SmartieEvent::Splashed:
                waterSplash(e.pos, 0.8f, 2);
                sfx(frand() < 0.5f ? "pi_swrm1" : "pi_swrm2", e.pos, 0.9f);
                LOGI("Smartie %d fell in the water", e.index);
                break;
            default:
                break;
        }
    }
}

void GameApp::sfx2D(const char* name, float vol, float pitch) {
    if (audio_ && ensureSound(name)) audio_->play2D(name, vol, pitch);
}

void GameApp::initCharacters(AssetWorld* w, ModelCache* mc) {
    const ObjectRegistry& reg = w->registry();
    size_t reapers = 0, rippers = 0, ambient = 0;
    bool raceMission = false;
    for (const auto& o : w->mission().objects) raceMission |= toLower(reg.modelName(o.type)) == "buoy_spline";
    // 1a: Timmy is up on his tower fending off the Rippers round it (user's description of the
    // original): his jetpack-swinging clip, and those Rippers stay above ground, besieging it.
    const bool timmyTower = toLower(w->missionName()) == "1a_1_timmy" && w->fs().exists("timmy_swing_jetpack.anm");
    Vec3 timmyAt{0, -1e6f, 0};
    for (const auto& ch : w->characters())
        if (toLower(ch.model).rfind("smtimmy", 0) == 0) timmyAt = ch.position;
    for (const auto& ch : w->characters()) {
        if (ch.objectClass == 51 && raceMission && (ch.group == 11 || ch.group == 12)) continue;  // race riders / starter (game_app_jetski.cpp)
        if (ch.objectClass == 51) {  // Reaper grunt: the Reaper AI, at its mission position
            const int ei = enemies_.spawn(ch.position, ch.yaw);
            if (ch.posted) {
                enemies_.enemies[size_t(ei)].posted = true;
                enemies_.enemies[size_t(ei)].postY = ch.position.y;
                enemies_.enemies[size_t(ei)].pos.y = ch.position.y;
            }
            if (ch.trigger == 2) {
                enemies_.enemies[size_t(ei)].objective = int(objectives_.size());
                objectives_.push_back({2, ch.position, ch.position});
            }
            missionHostiles_ = true;
            ++reapers;
            continue;
        }
        std::string idle = reg.modelAnim(ch.type);
        // Hanging from a hardpoint by one hand (Tel from his crashed plane in 1c_1): the
        // Meccaryn's one-handed hang clip, with that hand on the point.
        const bool hanging = !ch.hold.empty() && toLower(ch.model).rfind("mc_", 0) == 0 && w->fs().exists("mc_hang_onehand_yell.anm");
        if (hanging) idle = "mc_hang_onehand_yell.anm";
        if (timmyTower && toLower(ch.model).rfind("smtimmy", 0) == 0) {
            idle = "timmy_swing_jetpack.anm";
            LOGI("Timmy fends off the Rippers from his tower (timmy_swing_jetpack)");
        }
        auto m = std::make_unique<SkinnedModel>();
        if (idle.empty() || !m->load(w->fs(), *mc, renderer_, ch.model, idle)) continue;
        CharacterInst ci;
        ci.type = int(ch.type);
        ci.pos = ch.position;
        ci.yaw = ch.yaw;
        ci.scale = ch.scale;
        if (ch.objectClass == 45 || toLower(ch.model).rfind("dactyl", 0) == 0) {
            ci.flyer = true;
            ci.home = ch.position;
            ci.flyR = 25.0f + 25.0f * frand();
            ci.flyH = 14.0f + 12.0f * frand();
            ci.flySpeed = 8.0f + 3.0f * frand();
            ci.flyDir = frand() < 0.5f ? 1.0f : -1.0f;
            ci.flyAngle = frand() * 6.283f;
            ci.pos = ci.home + Vec3{ci.flyR * std::cos(ci.flyAngle), ci.flyH, ci.flyR * std::sin(ci.flyAngle)};
            LOGI("Flyer %s circles game (%.0f, %.0f), radius %.0f m, %.0f m up", ch.model.c_str(), -ci.home.x / gGameUnitsToMeters,
                 ci.home.z / gGameUnitsToMeters, ci.flyR, ci.flyH);
        }
        if (ch.trigger == 3 && ch.objectClass == 18) {
            // A stranded Smartie to carry to the pad (1b): its own clips for each state.
            static const char* clips[int(SmartieClip::Count)] = {"", "sm_wave.anm", "sm_squirm.anm", "sm_fall.anm",
                                                                  "sm_celebrate.anm", "sm_drowned.anm", "sm_hang.anm",
                                                                  "sm_hang_onearm.anm"};
            for (int c = 1; c < int(SmartieClip::Count); ++c) {
                const int k = m->addAnimation(w->fs(), clips[c]);
                ci.clipIndex[c] = k >= 0 ? k : 0;
            }
            ci.ambient = false;
            m->pose(renderer_, 0.0f);
            Vec3 mn, mx;
            m->posedBounds(mn, mx);
            ci.rescue = rescue_.add(ch.position, ch.yaw, (mx.y - mn.y) * ch.scale);
            ci.objective = int(objectives_.size());
            rescue_.smarties[size_t(ci.rescue)].objective = ci.objective;
            rescue_.smarties[size_t(ci.rescue)].slot = int(chars_.size());
            if (ch.hasHang) rescue_.setHang(ci.rescue, ch.hang);
            objectives_.push_back({3, ch.position, ch.position});
            ++ambient;
        } else if (ch.objectClass == 23) {
            // Ripper animation slots (xx_ripper.bin): 1 idle, 2 walk, 10 attack, 11 attack jump,
            // 44 hit, 49 small hit, 45 suffer (ends lying on the ground: the collapse death), 50 spit,
            // 41 popup (out of the ground), 42 buried idle, 43 dive (into it), 51 attack_mecc (latched on).
            static const int slots[int(CreatureClip::Count)] = {1, 2, 10, 11, 44, 49, 45, 50, 41, 42, 43, 51};
            for (int c = 1; c < int(CreatureClip::Count); ++c) {
                const std::string a = reg.anim(ch.type, slots[c]);
                const int k = a.empty() ? -1 : m->addAnimation(w->fs(), a);
                ci.clipIndex[c] = k >= 0 ? k : 0;
            }
            if (rippers == 0) {
                for (int c = 0; c < int(CreatureClip::Count); ++c)
                    creatures_.clipDuration[c] = std::max(0.1f, m->animDuration(ci.clipIndex[c]));
                // Shots hit the Ripper's body box (its idle-pose bounds, a little inside the legs'
                // reach) rather than an upright capsule: the body is long and low.
                m->pose(renderer_, 0.0f);
                Vec3 bmn, bmx;
                m->posedBounds(bmn, bmx);
                const Vec3 c0 = (bmn + bmx) * 0.5f, h = (bmx - bmn) * 0.5f;
                creatures_.params.boxMin = {c0.x - h.x * 0.7f, 0.0f, c0.z - h.z * 0.9f};
                creatures_.params.boxMax = {c0.x + h.x * 0.7f, (bmx.y - bmn.y) * 0.85f, c0.z + h.z * 0.9f};
                creatures_.params.hasBox = bmx.x > bmn.x && bmx.z > bmn.z;
                LOGI("Ripper hit box (scale 1): x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f m", creatures_.params.boxMin.x,
                     creatures_.params.boxMax.x, creatures_.params.boxMin.y, creatures_.params.boxMax.y, creatures_.params.boxMin.z,
                     creatures_.params.boxMax.z);
            }
            ci.ambient = false;
            // Three Ripper types share the model; their type records carry hit points 20 / 40 / 90
            // (type op 0x31) and the data has three sound sets (rs_/rm_/rl_) and two spit balls.
            const ObjectType* t = reg.type(ch.type);
            const float hp = t && t->hitPoints > 0 ? t->hitPoints : 40.0f;
            ci.size = hp <= 25.0f ? CreatureSize::Small : (hp <= 60.0f ? CreatureSize::Medium : CreatureSize::Large);
            const int idx = creatures_.spawn(ch.position, ch.yaw, ch.scale, ci.size, hp, true);
            // The placed Rippers start underground, under a mound, and come up when they feel the
            // player or a shot lands near - all but the small ones (user, 2026-10-25: "Rippers spawn
            // all except small ones underground"; nest and barracks broods come out above ground).
            // GVR_BURIED sets the share for tests.
            static const float buried = std::getenv("GVR_BURIED") ? float(std::atof(std::getenv("GVR_BURIED"))) : 1.0f;
            const bool besieging = length(Vec3{ch.position.x - timmyAt.x, 0, ch.position.z - timmyAt.z}) < 18.0f;
            if (ci.size != CreatureSize::Small && frand() < buried && !besieging) creatures_.bury(idx);
            if (ch.trigger == 2) {
                ci.objective = int(objectives_.size());
                objectives_.push_back({2, ch.position, ch.position});
            }
            creatures_.creatures[size_t(idx)].slot = int(chars_.size());
            missionHostiles_ = true;
            ++rippers;
        } else {
            ++ambient;
        }
        m->pose(renderer_, 0.0f);
        Vec3 mn, mx;
        m->posedBounds(mn, mx);
        ci.lift = -mn.y * ci.scale;
        ci.restLift = ci.lift;
        // Mission triggers on other characters: reaching one (trigger 1, e.g. Borjoyzee in
        // story3_borjoyzee_2), or killing one that has no AI here yet (trigger 2: Offspring, Charger).
        if (ch.trigger == 1) {
            objectives_.push_back({1, ch.position - Vec3{2, 0, 2}, ch.position + Vec3{2, 3, 2}});
            LOGI("Objective: reach %s", ch.model.c_str());
        }
        if (ch.objectClass == 1 && ci.ambient) {
            // Kabuto placed by a mission (the end of his story): the other giant, a fight.
            ci.model = std::move(m);
            setupBossAI(ci, 1);
            m = std::move(ci.model);
            ci.boss = true;
            // Full size, as the player's Kabuto (kb_l0's type scale, 0.25, would make him 4 m tall).
            ci.scale = 1.0f;
            ci.lift = ci.restLift = -mn.y;
            const ObjectType* t = reg.type(ch.type);
            ci.hp = ci.bossMaxHp = t && t->hitPoints > 0 ? t->hitPoints : 2500.0f;
            ci.bossR = std::max(1.5f, 0.12f * (mx.y - mn.y) * ci.scale);
            ci.bossH = std::max(2.0f, (mx.y - mn.y) * ci.scale);
            ci.objective = int(objectives_.size());
            objectives_.push_back({2, ch.position, ch.position});
            missionHostiles_ = true;
            LOGI("Objective: defeat Kabuto (%.0f hp, %.1f m tall)", ci.hp, ci.bossH);
        } else if (ch.trigger == 2 && ci.ambient && ch.objectClass != 18) {
            const ObjectType* t = reg.type(ch.type);
            ci.boss = true;
            ci.hp = t && t->hitPoints > 0 ? t->hitPoints : 300.0f;
            ci.bossR = std::max(0.6f, 0.35f * std::max(mx.x - mn.x, mx.z - mn.z) * ci.scale);
            ci.bossH = std::max(1.0f, (mx.y - mn.y) * ci.scale);
            ci.objective = int(objectives_.size());
            objectives_.push_back({2, ch.position, ch.position});
            missionHostiles_ = true;
            // The ones with clips for a fight get the boss AI: the Offspring (class 4), the Charger.
            const std::string lm = toLower(ch.model);
            const int kind = ch.objectClass == 4 || lm.rfind("offspring", 0) == 0 ? 2 : (lm == "charger" ? 3 : 0);
            if (kind) { ci.model = std::move(m); setupBossAI(ci, kind); m = std::move(ci.model); }
            LOGI("Objective: kill %s (%.0f hp, %.1f m tall%s) at game (%.0f, %.0f)", ch.model.c_str(), ci.hp, ci.bossH, kind ? ", fights" : "",
                 -ch.position.x / gGameUnitsToMeters, ch.position.z / gGameUnitsToMeters);
        }
        ci.time = frand() * 3.0f;
        Mat4 hand;
        if (hanging && m->nodeWorld(ch.hold, hand)) {
            const Vec3 local = giantsToRuntime(hand.m[12], hand.m[13], hand.m[14]) * ci.scale;
            ci.pos = ch.position - Quat::fromAxisAngle({0, 1, 0}, ci.yaw).rotate(local);
            ci.lift = 0;
            LOGI("%s hangs by %s at (%.1f %.1f %.1f)", ch.model.c_str(), ch.hold.c_str(), ch.position.x, ch.position.y, ch.position.z);
        }
        ci.model = std::move(m);
        chars_.push_back(std::move(ci));
    }
    // Ripper nests: a pool of Ripper models for the Rippers they breed (reused once a body is gone).
    nests_.clear();
    for (const auto& n : w->nests()) nests_.push_back({n.position, n.model, n.transform, n.radius, n.hitPoints, n.hitPoints, 1.0f, 1.0f,
                                                       2.0f + 3.0f * frand(), true, -1.0f});
    int ripBarracks = 0;  // rework: Ripper barracks (guard_ripperbarracks) breed medium Rippers like big nests
    for (const auto& o : w->worldObjects()) ripBarracks += o.name == "guard_ripperbarracks";
    if (!nests_.empty() || ripBarracks > 0) {
        // All nests are one type (ripnest, 100 hp); their size is the placement's scale. The
        // smaller ones (well under the level's biggest, or under 3 m across) hatch mini Rippers
        // (type 229, 20 hp, drawn at 0.55 scale); the big ones medium Rippers (type 73).
        float maxR = 0;
        for (const auto& n : nests_) maxR = std::max(maxR, n.radius);
        int bigNests = 0;
        for (auto& n : nests_) {
            n.small = n.radius < 0.8f * maxR || n.radius < 1.5f;
            bigNests += !n.small;
            LOGI("Ripper nest at (%.0f %.0f): radius %.2f m, %s", n.pos.x, n.pos.z, n.radius, n.small ? "small (mini Rippers)" : "big");
        }
        const int sources = int(nests_.size()) + ripBarracks;
        bigNests += ripBarracks;
        const int pool = std::min(12, 3 * sources);
        const int bigPool = bigNests == 0 ? 0 : (bigNests == sources ? pool : std::max(3, pool * bigNests / sources));
        for (int k = 0; k < pool; ++k) {
            const bool mini = k >= bigPool;
            const uint32_t type = mini ? 229 : 73;
            const std::string model = reg.modelName(type);
            auto m = std::make_unique<SkinnedModel>();
            if (model.empty() || !m->load(w->fs(), *mc, renderer_, model, reg.modelAnim(type))) break;
            CharacterInst ci;
            ci.type = int(type);
            static const int slots[int(CreatureClip::Count)] = {1, 2, 10, 11, 44, 49, 45, 50, 41, 42, 43, 51};
            for (int c = 1; c < int(CreatureClip::Count); ++c) {
                const std::string a = reg.anim(type, slots[c]);
                const int ki = a.empty() ? -1 : m->addAnimation(w->fs(), a);
                ci.clipIndex[c] = ki >= 0 ? ki : 0;
            }
            if (creatures_.creatures.empty() && k == 0)
                for (int c = 0; c < int(CreatureClip::Count); ++c)
                    creatures_.clipDuration[c] = std::max(0.1f, m->animDuration(ci.clipIndex[c]));
            const ObjectType* t = reg.type(type);
            ci.scale = (t && t->scale > 0 ? t->scale : 1.0f) * (mini ? 0.55f : 1.0f);
            ci.size = mini ? CreatureSize::Small : CreatureSize::Medium;
            ci.ambient = false;
            ci.visible = false;
            ci.pooled = true;
            m->pose(renderer_, 0.0f);
            Vec3 mn, mx;
            m->posedBounds(mn, mx);
            ci.lift = ci.restLift = -mn.y * ci.scale;
            ci.model = std::move(m);
            chars_.push_back(std::move(ci));
        }
        missionHostiles_ = true;
        LOGI("Ripper nests: %zu, %d pooled Rippers", nests_.size(), pool);
    }
    rescue_.groundHeight = enemies_.groundHeight;
    if (const char* h = std::getenv("GVR_SMARTIE_HOLD")) {  // test hook: how long Smarties hang on (s)
        rescue_.holdMin = rescue_.holdMax = float(std::atof(h));
        for (auto& sm : rescue_.smarties) sm.holdFor = rescue_.holdMin;
    }
    rescue_.hasPad = !rescue_.smarties.empty() && w->objectBounds("sm_dropoff", rescue_.padMin, rescue_.padMax);
    if (rescue_.hasPad) {
        // The pad's footprint plus a little: a Smartie set down on its edge counts.
        rescue_.padMin = rescue_.padMin - Vec3{0.5f, 0.5f, 0.5f};
        rescue_.padMax = rescue_.padMax + Vec3{0.5f, 1.5f, 0.5f};
        LOGI("Smartie pad: (%.1f %.1f %.1f) .. (%.1f %.1f %.1f); %zu Smarties to rescue", rescue_.padMin.x, rescue_.padMin.y,
             rescue_.padMin.z, rescue_.padMax.x, rescue_.padMax.y, rescue_.padMax.z, rescue_.smarties.size());
    }
    // Water effects and the piranha school (original models and clips).
    if (!splashFx_) {
        auto load = [&](const char* base, const char* anim) -> std::unique_ptr<SkinnedModel> {
            auto m = std::make_unique<SkinnedModel>();
            if (!m->load(w->fs(), *mc, renderer_, base, anim)) return nullptr;
            return m;
        };
        splashFx_ = load("efx_splash", "efx_splash.anm");
        ringFx_ = load("water_rings", "water_rings.anm");
        pirSwim_ = load("piranha", "pir_swim.anm");
        pirJump_ = load("piranha", "pir_swim_jump.anm");
        LOGI("Water effects: splash %d, rings %d, piranhas %d", splashFx_ != nullptr, ringFx_ != nullptr, pirSwim_ != nullptr);
    }
    creatures_.groundHeight = enemies_.groundHeight;
    creatures_.collision = enemies_.collision;
    // Spit leaves from the animated mouth node (ref_mouth) when the skeleton has one.
    creatures_.mouth = [this](int i, Vec3& out) {
        const Creature& c = creatures_.creatures[size_t(i)];
        if (c.slot < 0) return false;
        const CharacterInst& ci = chars_[size_t(c.slot)];
        Mat4 m;
        if (!ci.model->nodeWorld("ref_mouth", m)) return false;
        const Vec3 local = giantsToRuntime(m.m[12], m.m[13], m.m[14]) * ci.scale;
        out = ci.pos + Vec3{0, ci.lift, 0} + Quat::fromAxisAngle({0, 1, 0}, ci.yaw).rotate(local);
        return true;
    };
    if (rippers) {
        const char* balls[3] = {nullptr, "efx_ripper_mspit", "efx_ripper_lspit"};
        for (int k = 1; k < 3; ++k) {
            auto b = std::make_unique<SkinnedModel>();
            if (!b->load(w->fs(), *mc, renderer_, balls[k], std::string(balls[k]) + ".anm")) continue;
            b->pose(renderer_, 0.0f);
            Vec3 mn, mx;
            b->posedBounds(mn, mx);
            spitCentre_[k] = (mn + mx) * 0.5f;
            spitBall_[k] = std::move(b);
        }
        spitRing_[1] = mc->get("ripspit.gbs");
        spitRing_[2] = mc->get("ripspitbig.gbs");
        goreHead_ = mc->get("rip_gore_head.gbs", "", /*dynamicLit=*/true);
        goreArm_ = mc->get("rip_gore_arm.gbs", "", /*dynamicLit=*/true);
    }
    initGore(w, mc);
    initFx(mc);
    initDataFx();
    initBurrows(w, mc);
    initHud(mc);
    timmyGreeted_ = false;
    timmyLineT_ = 0;
    timmyChatCooldown_ = 4.0f;
    initPickups(mc);
    initShop();
    initBase();
    initPopup();
    initArsenal();
    initMines();
    initVillagers(w, mc);
    initVimps(w, mc);
    initBirds(w, mc);
    initHitchers(w, mc);
    initFish(w, mc);
    initSeaMonster(w, mc);
    initAmbience(w);
    initDestruction(w, mc);
    initSpawners();
    LOGI("Mission characters: %zu Rippers, %zu Reapers, %zu ambient", rippers, reapers, ambient);
    // The VR map's island model is built now, behind the loading screen (it took ~5 ms on the host
    // when built on the first frame of play).
    if (worldName_ != mapWorld_) mapReset();
    buildMapMesh();
}

void GameApp::updateCharacters(float dt) {
    GVR_PROF("chars (all)");
    {
        GVR_PROF("creatures ai");
        static const bool passive = std::getenv("GVR_PASSIVE_CREATURES") != nullptr;  // test hook: Rippers ignore the player
        creatures_.update(dt * enemyTimeScale(), player_.position, playMode_ && playerHealth_ > 0 && !dying_ && !passive, creatureEvents_);
    }
    for (const auto& e : creatureEvents_) {
        creatureSound(e);
        burrowEvent(e);
        if (e.kind == CreatureEvent::SpitLaunch) ++spitsLaunched_;
        if (e.kind == CreatureEvent::SpitBurst && e.damage > 0.5f) ++spitHits_;
        if ((e.kind == CreatureEvent::Strike || e.kind == CreatureEvent::SpitBurst) && e.damage > 0.5f)
        {
            hitFrom_ = e.pos;
            hitDirTimer_ = 1.2f;
            damagePlayer(e.damage, e.kind == CreatureEvent::Strike ? DeathCause::Bitten : DeathCause::Blast);
        }
        if (e.kind == CreatureEvent::SpitBurst) blasts_.push_back({e.pos, int(e.size), 0.0f});
    }
    // Rippers: the model follows its creature; burst or sunk-away bodies are hidden.
    for (auto& ci : chars_)
        if (!ci.ambient) ci.visible = false;
    for (auto& c : creatures_.creatures) {
        if (c.slot < 0 || size_t(c.slot) >= chars_.size() || c.exploded) continue;
        CharacterInst& ci = chars_[size_t(c.slot)];
        if (CreatureDirector::underground(c)) continue;  // a mound (renderBurrows), not drawn
        ci.visible = true;
        ci.pos = c.pos;
        ci.yaw = c.yaw;
        if (c.state == CreatureState::Latched) {
            // Riding the player: at the legs or the waist (latchOffset, turned with his body),
            // facing in; the creature stands where it hangs so it drops off there.
            const Vec3 off = Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(Vec3{c.latchOffset.x, 0, c.latchOffset.z});
            ci.pos = player_.position + off + Vec3{0, c.latchOffset.y, 0};
            ci.yaw = std::atan2(off.x, off.z);  // facing the player (-Z forward: toward -off)
            c.pos = Vec3{ci.pos.x, player_.position.y, ci.pos.z};
            c.yaw = ci.yaw;
        }
        const float dist = length(c.pos - player_.position);
        if (dist > 120.0f) continue;  // too far to see the animation: skip skinning
        ci.model->poseBlend(renderer_, ci.clipIndex[int(c.prevClip)], c.prevTime, ci.clipIndex[int(c.clip)], c.clipTime,
                            c.fadeWeight(creatures_.params.crossfade));
        Vec3 mn, mx;
        ci.model->posedBounds(mn, mx);
        // The burrowing clips move the body below the ground themselves (rip_popup rises from
        // 2 m down, rip_dive sinks): the model stays at its standing height for those.
        const bool dig = c.clip == CreatureClip::Popup || c.clip == CreatureClip::BuriedIdle || c.clip == CreatureClip::Dive ||
                         c.clip == CreatureClip::Latch;
        if (dig) ci.lift = ci.restLift;
        else if (!npcRagdolled(kNpcCreature, c.slot)) ci.lift += (-mn.y * ci.scale - ci.lift) * std::min(1.0f, dt / 0.06f);  // (a ragdoll keeps its frame)
        // Footsteps while running.
        if (c.clip == CreatureClip::Walk && c.speed > 0.5f && dist < 30.0f) {
            ci.stepTimer -= dt * c.rate;
            if (ci.stepTimer <= 0) {
                ci.stepTimer = 0.45f;
                const char* feet[3] = {"rs_feet", "rm_feet", "rl_feet"};
                sfx(feet[int(ci.size)], c.pos, 0.55f, 0.9f + 0.2f * frand());
            }
        }
    }
    updateBurrows(dt);
    // Spit balls wobble (their own clip); blasts and gore age.
    for (auto& b : spitBall_)
        if (b) b->pose(renderer_, float(time_));
    for (auto& b : blasts_) b.age += dt;
    blasts_.erase(std::remove_if(blasts_.begin(), blasts_.end(), [](const Blast& b) { return b.age > 0.6f; }), blasts_.end());
    for (auto& g : gibs_) {
        if (g.hand >= 0) { g.life = 0; continue; }  // (in a hand: game_app_stones.cpp moves it)
        g.life += dt;
        g.vel.y -= 14.0f * dt;
        g.pos += g.vel * dt;
        // Rework: a gun or a limb thrown by hand hits the first Reaper it meets (a gun hard, a limb less).
        if (g.thrown && length(g.vel) > 4.0f)
            for (size_t k = 0; k < enemies_.enemies.size(); ++k) {
                const Enemy& e = enemies_.enemies[k];
                if (e.state == EnemyState::Dead || std::hypot(e.pos.x - g.pos.x, e.pos.z - g.pos.z) > 0.6f || g.pos.y < e.pos.y ||
                    g.pos.y > e.pos.y + enemies_.bodyHeight(e))
                    continue;
                const float sp = length(g.vel);
                enemies_.push(int(k), normalize(Vec3{g.vel.x, 0, g.vel.z} + Vec3{0.001f, 0, 0}) * std::min(4.0f, 0.4f * sp));
                const bool killed = thrownHitEnemy(int(k), (g.owner ? 6.0f + 1.6f * sp : 3.0f + 0.8f * sp), g.pos, normalize(g.vel));
                sfx(g.owner ? "ex_metls" : "ex_bdys1", g.pos, 0.9f, 1.0f);
                LOGI("A thrown %s hits Reaper %u at %.1f m/s%s", g.owner ? "gun" : "piece", e.uid, sp, killed ? " - he's down" : "");
                g.vel = g.vel * -0.2f;
                g.thrown = false;
                break;
            }
        if (g.thrown && length(g.vel) < 2.0f) g.thrown = false;
        g.angle += g.spin * dt;
        const float ground = enemies_.groundHeight ? enemies_.groundHeight(g.pos.x, g.pos.z) : 0.0f;
        if (g.life > 18.0f) {  // sink away
            g.pos.y -= 0.4f * dt;
            continue;
        }
        if (g.pos.y < ground + 0.1f * g.scale) {  // bounce, then settle
            // Rework: it lands with a sound - a gun clatters, a limb or a piece thuds.
            if (g.vel.y < -3.0f && length(g.pos - player_.position) < 40.0f) {
                const bool gun = g.model && (g.model == rpWeapon_[0].model || g.model == rpWeapon_[1].model);
                sfx(gun ? (frand() < 0.5f ? "me_drpwp" : "ex_metls") : (frand() < 0.5f ? "ex_bdys1" : "ex_bdys2"), g.pos,
                    std::min(1.0f, 0.2f - g.vel.y * 0.08f), gun ? 1.0f + 0.1f * frand() : 1.1f + 0.2f * frand());
            }
            g.pos.y = ground + 0.1f * g.scale;
            g.vel = Vec3{g.vel.x * 0.5f, std::fabs(g.vel.y) * 0.3f, g.vel.z * 0.5f};
            g.spin *= 0.5f;
        }
    }
    gibs_.erase(std::remove_if(gibs_.begin(), gibs_.end(), [](const Gib& g) { return g.life > 20.0f; }), gibs_.end());
    // Rework: a Reaper whose gun was shot out of his hand goes back for it when you're not right on him
    // (within 10 m of it, while it lies there - 15 s); at it, he picks it up and fights on.
    for (auto& e : enemies_.enemies) {
        if (!e.disarmed || e.gunArmOff || e.state == EnemyState::Dead) { e.fetchGun = false; continue; }
        Gib* gun = nullptr;
        for (auto& g : gibs_)
            if (g.owner == e.uid && g.life < 15.0f) gun = &g;
        const float toGun = gun ? length(Vec3{gun->pos.x - e.pos.x, 0, gun->pos.z - e.pos.z}) : 1e9f;
        const float toPlayer = length(Vec3{player_.position.x - e.pos.x, 0, player_.position.z - e.pos.z});
        const bool was = e.fetchGun;
        e.fetchGun = gun && gun->life > 1.5f && toGun < 10.0f && toPlayer > 2.5f;  // (a moment of surprise first)
        if (!e.fetchGun) continue;
        e.gunAt = gun->pos;
        if (!was) LOGI("Reaper %u runs for his gun (%.1f m)", e.uid, toGun);
        if (toGun < 0.9f) {
            gun->life = 1e9f;  // (gone from the ground: in his hand)
            e.disarmed = e.fetchGun = false;
            e.weaponOn = true;
            sfx("me_drpwp", e.pos + Vec3{0, 0.5f, 0}, 0.7f, 1.3f);
            LOGI("Reaper %u picks his gun up", e.uid);
        }
    }
    gibs_.erase(std::remove_if(gibs_.begin(), gibs_.end(), [](const Gib& g) { return g.life > 1e8f; }), gibs_.end());
    for (auto& ci : chars_) {
        if (ci.rescue < 0) continue;
        const RescueSmartie& r = rescue_.smarties[size_t(ci.rescue)];
        ci.visible = r.visible();
        ci.pos = r.pos;
        ci.yaw = r.yaw;
        ci.time = r.stateTime;
        ci.lift = ci.restLift;
        if (ci.visible && length(ci.pos - player_.position) < 150.0f) {
            const int k = ci.clipIndex[int(r.clip)];
            const float d = ci.model->animDuration(k);
            // Falling and drowning play once and hold; the rest loop.
            const bool once = r.clip == SmartieClip::Fall || r.clip == SmartieClip::Drowned;
            const float t = once ? std::min(r.stateTime, d * 0.999f) : float(time_ + 0.37 * ci.rescue);
            ci.model->poseBlend(renderer_, k, t, k, t, 0.0f);
            // Hanging: its hands on the hang point (both hands, or the one still holding on).
            Mat4 lh, rh;
            if (r.state == SmartieState::Hanging && ci.model->nodeWorld("l_hand", lh) && ci.model->nodeWorld("r_hand", rh)) {
                Vec3 g{rh.m[12], rh.m[13], rh.m[14]};
                if (r.clip == SmartieClip::Hang) g = (g + Vec3{lh.m[12], lh.m[13], lh.m[14]}) * 0.5f;
                const Vec3 local = giantsToRuntime(g.x, g.y, g.z) * ci.scale;
                ci.pos = r.hangPoint - Quat::fromAxisAngle({0, 1, 0}, ci.yaw).rotate(local);
                ci.lift = 0;
            }
        }
    }
    // Ambient characters loop their default clip (only animated when near).
    for (auto& ci : chars_) {
        if (!ci.ambient || ci.bossKind > 0) continue;  // bosses with AI: updateBosses
        ci.time += dt;
        if (ci.flyer) {
            // A wide, slowly breathing circle round its home, rising and dipping; banked into the
            // turn (its own wing-beat clip plays on top). Our flight path: the data places it only.
            const float r = ci.flyR * (1.0f + 0.25f * std::sin(ci.time * 0.13f + ci.home.x));
            ci.flyAngle += ci.flyDir * ci.flySpeed / std::max(5.0f, r) * dt;
            const float a = ci.flyAngle;
            const float h = ci.flyH + 4.0f * std::sin(ci.time * 0.31f + ci.home.z);
            const Vec3 next = ci.home + Vec3{r * std::cos(a), h, r * std::sin(a)};
            const Vec3 v = next - ci.pos;
            if (length(Vec3{v.x, 0, v.z}) > 1e-4f) {
                ci.yaw = std::atan2(-v.x, -v.z);
                const float bank = std::atan(ci.flySpeed * ci.flySpeed / (std::max(5.0f, r) * 9.81f));
                ci.roll = -ci.flyDir * std::min(0.6f, bank);
                ci.pitch = std::max(-0.3f, std::min(0.3f, std::atan2(v.y, length(Vec3{v.x, 0, v.z}))));
            }
            ci.pos = next;
            ci.lift = 0;
        }
        if (length(ci.pos - player_.position) < (ci.flyer ? 200.0f : 120.0f)) ci.model->pose(renderer_, ci.time);
    }
}

void GameApp::updateTimmy() {
    timmyOn_ = false;
    if (!timmy_ || !playerModel_ || worldName_ != "Story1" || !player_.jetpackAvailable) { recentKills_ = 0; return; }
    Mat4 spine, mount, centre;
    // Idle loop; in flight a yell now and then (played once, cross-faded in and out).
    const float dt = std::max(0.0f, float(time_) - timmyLastTime_);
    timmyLastTime_ = float(time_);
    timmyIdleT_ += dt;
    if (timmyClip_ > 0) {
        timmyClipT_ += dt;
        if (timmyClipT_ >= timmy_->animDuration(timmyClip_)) { timmyClip_ = 0; timmyClipT_ = 0; timmyFade_ = 0; }
    } else if (!player_.onGround && player_.thrusting()) {
        timmyYellIn_ -= dt;
        if (timmyYellIn_ <= 0) {
            const int k = timmyYell_[int(frand() * 2.99f)];
            if (k > 0) {
                timmyClip_ = k; timmyClipT_ = 0; timmyFade_ = 0;
                timmySay(frand() < 0.5f ? "smchride3" : "smchride1");  // "Do that jetpack thing again!" / "Uh, where are you taking me?"
            }
            timmyYellIn_ = 5.0f + 6.0f * frand();
        }
    }
    timmyFade_ = std::min(1.0f, timmyFade_ + dt / 0.25f);
    if (timmyClip_ > 0) timmy_->poseBlend(renderer_, 0, timmyIdleT_, timmyClip_, timmyClipT_, timmyFade_);
    else timmy_->poseBlend(renderer_, timmyPrevClip_, std::min(timmyPrevT_, timmy_->animDuration(timmyPrevClip_) - 0.01f), 0, timmyIdleT_, timmyFade_);
    if (timmyClip_ > 0) { timmyPrevClip_ = timmyClip_; timmyPrevT_ = timmyClipT_; }
    if (!playerModel_->nodeWorld("spine", spine) || !timmy_->nodeWorld("center", centre)) return;
    // Mount point: Baz's Ref_HolsterB (where he rode in device tests 20-21) when the clip has it,
    // remembered relative to the spine; else the spine with that offset. Clips without reference
    // nodes (mc_hover, mc_fly_left...) used to drop him (device test 23: "keeps disappearing when
    // flying or changing direction").
    if (playerModel_->nodeWorld("ref_holsterb", mount)) {
        smartieFromSpine_ = affineInverse(spine) * mount;
        haveSmartieLocal_ = true;
    } else if (haveSmartieLocal_) {
        mount = spine * smartieFromSpine_;
    } else {
        return;
    }
    if (std::getenv("GVR_TIMMY_TRACE")) LOGI("timmy: anim %d mount (%.2f %.2f %.2f)", int(player_.anim()), mount.m[12], mount.m[13], mount.m[14]);
    // Orientation: Baz's. Timmy sits facing backwards, watching what is behind Baz (device test 23;
    // sm_* bodies face backwards in their own frame, so no turn).
    const Vec3 mPos = giantsToRuntime(mount.m[12], mount.m[13], mount.m[14]);
    const Vec3 cPos = giantsToRuntime(centre.m[12], centre.m[13], centre.m[14]);
    float turn = 0.0f;
    if (const char* t = std::getenv("GVR_TIMMY_TURN")) turn = std::atof(t) * kPi / 180.0f;  // test hook
    // Lean: the spine's rotation now relative to its idle rotation (model frame), so he tilts with
    // Baz's torso - forward in forward flight, sideways when banking (device test 24).
    Mat4 lean = gameMatrixToRuntime(spine);
    for (int c = 0; c < 3; ++c) {
        const float l = std::sqrt(lean.m[c * 4] * lean.m[c * 4] + lean.m[c * 4 + 1] * lean.m[c * 4 + 1] + lean.m[c * 4 + 2] * lean.m[c * 4 + 2]);
        if (l > 1e-6f)
            for (int r = 0; r < 3; ++r) lean.m[c * 4 + r] /= l;
    }
    lean.m[12] = lean.m[13] = lean.m[14] = 0;
    lean = lean * timmyRestFix_;
    if (std::getenv("GVR_TIMMY_TRACE")) {
        const float tr = lean.m[0] + lean.m[5] + lean.m[10];
        LOGI("timmy lean: anim %d angle %.0f deg, up -> (%.2f %.2f %.2f)", int(player_.anim()),
             std::acos(std::max(-1.0f, std::min(1.0f, 0.5f * (tr - 1.0f)))) * 57.3f, lean.m[4], lean.m[5], lean.m[6]);
    }
    timmyXf_ = bodyXf_ * Mat4::translation(mPos + timmyOffset_) * lean * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, turn)) *
               Mat4::translation(cPos * -1.0f);
    timmyOn_ = true;
    timmyChatCooldown_ = std::max(0.0f, timmyChatCooldown_ - dt);
    if (!timmyGreeted_ && timmyIdleT_ > 2.0f) { timmyGreeted_ = true; timmySay("smchpickup2", true); }  // "I've hopped on!"
    if (recentKills_ > 0) {
        if (frand() < 0.5f) timmySay("smchride2");  // "You're one cool geezer!"
        recentKills_ = 0;
    }
    // The Timmy who stood on his tower in 1a is now on the back.
    for (auto& c : chars_)
        if (c.ambient && c.type == timmyType_) c.visible = false;
}

void GameApp::timmySay(const char* key, bool force) {
    if (!timmy_ || !player_.jetpackAvailable || worldName_ != "Story1" || (!force && timmyChatCooldown_ > 0) || cutsceneActive()) return;
    std::string line = gameText_ ? gameText_->get(key) : std::string();
    if (line.empty()) return;
    for (auto& ch : line)
        if (static_cast<unsigned char>(ch) == 0x92) ch = '\'';  // the text's Windows apostrophe
    timmyLine_ = "Timmy: " + line;
    timmyLineT_ = 3.2f;
    timmyChatCooldown_ = 10.0f + 6.0f * frand();
    // A Smartie voice clip from Timmy's seat, pitched up for a child.
    static const int voices[] = {1, 2, 3, 4, 13, 14};
    char n[16];
    std::snprintf(n, sizeof n, "gsm%02d_%d", voices[int(frand() * 5.99f)], frand() < 0.5f ? 1 : 2);
    const Vec4 p = timmyXf_ * Vec4{0, 0, 0, 1};
    sfx(n, {p.x, p.y, p.z}, 1.0f, 1.18f);
    LOGI("Timmy says (%s): %s", key, line.c_str());
}

// Timmy's line as a subtitle: head-locked below the line of sight (as the cutscene subtitles).
void GameApp::renderTimmyLine() {
    if (timmyLineT_ <= 0 || timmyLine_.empty() || !text_.ready() || !subInit_ || cutsceneActive() || !settings_.subtitles) return;
    float w = 0, h = 0;
    const MeshId m = text_.mesh(timmyLine_, 0.045f, 1.3f, &w, &h);
    if (!m) return;
    const float a = std::min(1.0f, timmyLineT_ / 0.4f);
    const Quat r = subRot_;
    const Vec3 at = rig_.toWorld(lastInput_.head).position + r.rotate(Vec3{0.5f * w + 0.04f, -0.30f, -1.4f});  // right of centre: clear of the HUD
    const Mat4 xf = Mat4::translation(at) * Mat4::rotation(r * Quat::fromAxisAngle({1, 0, 0}, 0.22f));
    text_.drawPanel(xf, w + 0.08f, h + 0.05f, {0, 0, 0, 0.5f * a});
    text_.draw(m, xf * Mat4::translation({0, 0, 0.003f}), {0.75f, 1.0f, 0.75f, a});
}

void GameApp::loadWorldQuality() {
    if (settingsPath_.empty()) return;
    const std::string path = settingsPath_.substr(0, settingsPath_.rfind('/') + 1) + "quality2.txt";
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) worldQuality_[toLower(line.substr(0, eq))] = std::max(0, std::min(3, std::atoi(line.c_str() + eq + 1)));
    }
}

void GameApp::saveWorldQuality() const {
    if (settingsPath_.empty()) return;
    const std::string path = settingsPath_.substr(0, settingsPath_.rfind('/') + 1) + "quality2.txt";
    std::ofstream f(path, std::ios::trunc);
    for (const auto& [w, q] : worldQuality_) f << w << "=" << q << "\n";
}

void GameApp::updateQuality(float dt) {
    static const int pinned = std::getenv("GVR_QUALITY") ? std::atoi(std::getenv("GVR_QUALITY")) : -1;  // test hook: fixed level
    if (pinned >= 0) { quality_ = std::min(3, pinned); return; }
    // Loading and the first seconds after it don't count: on the Quest the first 3-4 s of a level ran
    // at 35-50 fps (first use of its textures and shaders), which used to drop the quality for good.
    if (load_.phase != LoadPhase::Idle || cutsceneActive()) { perfIgnore_ = 6.0f; perfTime_ = 0; perfFrames_ = 0; return; }
    if (worldName_ != qualityWorld_) {
        if (!qualityWorld_.empty()) worldQuality_[toLower(qualityWorld_)] = quality_;
        qualityWorld_ = worldName_;
        const std::string w = toLower(worldName_);
        auto it = worldQuality_.find(w);
        // Measured on the Quest 2 (2026-10-25 device runs): Kabuto's story 3 world held 72 Hz only at
        // level 2, and so did Reaper story 2 once its jet-ski races and first battle got going (it stepped down to 2 in all three, 9ed3505).
        // 2026-10-26 (run 20261026-233657, shader variants + haze cube): level 1 held 72 Hz in the 5a meadow,
        // Kabuto's k1a and Delphi's r1; 1a's village needed 2, Kabuto's storm (Kstory5) 3. Elsewhere start at 1
        // (level 0, the full look, rarely fits; the fallback steps up to it after 12 s at full rate).
        const int measured = w == "kstory5" ? 3 : (w == "story1" || w == "kstory3" || w == "rstory2") ? 2 : 1;
        quality_ = std::max(measured, it != worldQuality_.end() ? it->second : 0);
        perfGood_ = 0;
        upHold_ = 0;
        sinceUp_ = 1e9f;
        if (quality_ > 0) LOGI("perf: %s starts at effects quality level %d", worldName_.c_str(), quality_);
    }
    sinceUp_ += dt;
    upHold_ = std::max(0.0f, upHold_ - dt);
    if (perfIgnore_ > 0) { perfIgnore_ -= dt; return; }
    if (dt <= 0 || dt > 0.05f) return;  // a single hitch
    perfTime_ += dt;
    ++perfFrames_;
    if (perfTime_ < 2.0f) return;  // (2 s windows: on the headset a 4 s window let a scene run at 36 fps for 8-12 s first)
    const float avg = perfTime_ / float(perfFrames_);
    // Down below 69 fps (was 65: a scene at 68 never stepped down and stuttered on).
    if (avg > 1.0f / 69.0f && quality_ < 3) {
        // Far below (under 50 fps): two steps at once - one a window left a heavy scene stuttering for 10 s+.
        quality_ = std::min(3, quality_ + (avg > 1.0f / 50.0f ? 2 : 1));
        perfGood_ = 0;
        if (sinceUp_ < 10.0f) upHold_ = 600.0f;  // the last step up didn't hold: not again for 10 min (it oscillated)
        LOGW("perf: %.1f fps average, effects quality down to level %d", 1.0f / avg, quality_);
        worldQuality_[toLower(worldName_)] = std::max(quality_, worldQuality_[toLower(worldName_)]);
        saveWorldQuality();
    } else if (avg < 1.0f / 71.0f && quality_ > 0 && upHold_ <= 0) {
        // Back up after 12 s at full rate (six windows in a row).
        if (++perfGood_ >= 6) {
            --quality_;
            perfGood_ = 0;
            sinceUp_ = 0;
            LOGI("perf: %.1f fps average, effects quality back up to level %d", 1.0f / avg, quality_);
        }
    } else {
        perfGood_ = 0;
    }
    perfTime_ = 0;
    perfFrames_ = 0;
}

// A start marker on top of a mission character (r1_saveyan: Delphi began inside Yan, whose wave
// then put his face in hers): step back along the facing until there is room, the character in front.
Vec3 GameApp::clearSpawn(Vec3 sp, float yaw) const {
    if (hero_ == Hero::Kabuto || !assetWorld_) return sp;
    const Vec3 back{std::sin(yaw), 0, std::cos(yaw)};
    for (int step = 0; step < 6; ++step) {
        bool crowded = false;
        for (const auto& c : chars_)
            if (c.visible && !c.rider && length(Vec3{c.pos.x - sp.x, 0, c.pos.z - sp.z}) < 1.1f) crowded = true;
        for (const auto& r : rescue_.smarties)
            if (length(Vec3{r.pos.x - sp.x, 0, r.pos.z - sp.z}) < 1.1f) crowded = true;
        if (!crowded) break;
        sp = sp + back * 0.5f;
        float gy;
        if (assetWorld_->terrain().heightAt(sp.x, sp.z, gy)) sp.y = std::max(gy, sp.y - 1.0f);
        if (step == 0) LOGI("Spawn: a character stands on the start marker - stepping back");
    }
    return sp;
}

// Gun muzzle, barrel direction and handle in the gun model's GAME frame (forward = -Y like
// the characters; up = +Z).
static const Vec3 kGunForwardGame{0.0f, -1.0f, 0.0f};

void GameApp::updateThirdPersonAim(const InputFrame& in, float dt) {
    const ControllerState& c = in.hand[rig_.turnHand()];
    if (!c.aimPoseValid) { tpAiming_ = false; return; }
    const Pose aim = rig_.toWorld(c.aimPose);
    const Vec3 dir = aim.orientation.rotate(Vec3{0, 0, -1});
    float t;
    Vec3 n;
    int target = -1;
    aimTarget_ = (weapon_.raycast && weapon_.raycast(aim.position, dir, 150.0f, t, n, target)) ? aim.position + dir * t
                                                                                                : aim.position + dir * 60.0f;
    if (c.trigger > 0.1f) tpAimHold_ = 1.0f;  // keep aiming briefly after releasing
    else tpAimHold_ = std::max(0.0f, tpAimHold_ - dt);
    tpAiming_ = tpAimHold_ > 0.0f;
}

void GameApp::gunKick(float strength) { gunKickV_ += strength * (twoHand_ ? 6.0f : 12.0f); }

// A damped spring back to rest (about 0.25 s).
void GameApp::updateGunKick(float dt) {
    const float a = -260.0f * gunKick_ - 26.0f * gunKickV_;
    gunKickV_ += a * dt;
    gunKick_ += gunKickV_ * dt;
    gunKick_ = std::max(-0.1f, std::min(0.6f, gunKick_));
}

// The held gun as drawn: rotated muzzle-up about its handle by the kick and pushed back a little.
Mat4 GameApp::gunDrawXf() const {
    if (!firstPerson_ || std::fabs(gunKick_) < 1e-4f) return gunXf_;
    Vec3 handleGame, muzzleGame;
    heldRefs(handleGame, muzzleGame);
    const Vec3 handle = giantsToRuntime(handleGame.x, handleGame.y, handleGame.z);
    const Vec3 fr = normalize(giantsToRuntime(kGunForwardGame.x, kGunForwardGame.y, kGunForwardGame.z));
    const Vec3 up = normalize(giantsToRuntime(0, 0, 1));
    Vec3 axis = cross(fr, up);
    if (length(axis) < 1e-4f) return gunXf_;
    axis = normalize(axis);
    const float back = gunKick_ * 0.35f * length(muzzleGame - handleGame) * gGameUnitsToMeters;
    return gunXf_ * Mat4::translation(handle - fr * back) * Mat4::rotation(Quat::fromAxisAngle(axis, gunKick_)) * Mat4::translation(handle * -1.0f);
}

void GameApp::updateWeapon(float dt, const InputFrame& in) {
    const int gh = rig_.turnHand();
    if (weaponSel_ != 10) repairOn_ = false;  // (2026-10-28 review: the beam stayed drawn after a switch to the bush or bombs)
    if (weaponSel_ != 11) lastWeaponSel_ = weaponSel_;
    updatePortTurret(dt, in);  // (planted: it fights on whatever is in hand)
    updateGunKick(dt);
    // Right stick forward: next weapon (released to neutral before it switches again).
    const float sy = in.hand[gh].thumbstick.y;
    // (2026-10-28) Held forward 0.4 s with three or more things carried: the weapon wheel; a shorter push steps on.
    int pick = -1;
    if (sy > 0.75f && (switchArmed_ || stickFwdT_ > 0)) {  // (a push begun from neutral, held)
        stickFwdT_ += dt;
        if (!wheelOpen_ && stickFwdT_ > 0.4f && in.hand[gh].gripPoseValid) {
            buildWheelCells();
            if (wheelCells_.size() >= 3) openWheel(in, gh);
        }
    }
    if (wheelOpen_) {
        updateWheelHover(in, gh);
        if (sy < 0.3f) {
            wheelOpen_ = false;
            if (wheelHover_ >= 0 && size_t(wheelHover_) < wheelCells_.size()) pick = wheelCells_[size_t(wheelHover_)].sel;
            LOGI("Weapon wheel closed (%s)", pick >= 0 ? "chosen" : "nothing chosen");
        }
    }
    bool step = false;
    if (sy < 0.3f) {
        if (!switchArmed_ && stickFwdT_ > 0 && pick < 0 && !wheelWasOpen_) step = true;  // (a short push)
        switchArmed_ = true;
        stickFwdT_ = 0;
        wheelWasOpen_ = false;
    } else if (sy > 0.75f) {
        switchArmed_ = false;
        if (wheelOpen_) wheelWasOpen_ = true;
    }
    if (step || pick >= 0) {
        // The weapon strip drops in either way (showing what is carried); with only the gun
        // there is nothing to switch to.
        if (pick >= 0) {
            if (weaponOwned(pick)) weaponSel_ = pick;
        } else {
            // Handgun -> shotgun -> machine gun -> sniper gun -> mortar -> grenades -> launcher -> bush pack ->
            // pop-up bomb, skipping what he doesn't carry.
            int at = 0;
            while (at < kNumSel && kWeaponOrder[at] != weaponSel_) ++at;
            for (int k = 1; k <= kNumSel; ++k) {
                const int next = kWeaponOrder[(at + k) % kNumSel];
                if (weaponOwned(next)) { weaponSel_ = next; break; }
            }
        }
        if (weaponSel_ == 9) {
            timmyLine_ = "MINES (" + std::to_string(mines_) + ") - TRIGGER TO SET ONE DOWN";
            timmyLineT_ = 3.0f;
        }
        if (weaponSel_ == 8) {
            timmyLine_ = "GRENADES (" + std::to_string(grenades_) + ") - HOLD THE TRIGGER, THROW, LET GO";
            timmyLineT_ = 3.0f;
        }
        if (weaponSel_ == 3) {
            timmyLine_ = "POP-UP BOMB (" + std::to_string(popupBombs_) + ") - TRIGGER TO SET IT DOWN";
            timmyLineT_ = 3.0f;
        }
        weaponHudT_ = 0;
        sfx2D("ph_selct", 0.7f);
        haptics_.push_back({gh, 0.5f, 0.06f});
        const int gs = gunOfSel(weaponSel_);
        if (gs >= 0) {
            timmyLine_ = std::string(gs == kGunShotgun ? "SHOTGUN" : gs == kGunMachine ? "MACHINE GUN" : gs == kGunSniper ? "SNIPER GUN" : "MILLENIUM MORTAR") + " (" +
                         std::to_string(guns_[gs].ammo) + ")";
            timmyLineT_ = 2.0f;
        }
        if (mgLoop_ && audio_) { audio_->stop(mgLoop_); mgLoop_ = 0; }
        static const char* kSelNames[kNumSel] = {"gun", "rocket launcher", "bush pack", "pop-up bomb", "", "", "", "", "grenades", "mines", "repair gun", "turret"};
        LOGI("Weapon: %s", gs >= 0 ? guns_[gs].name : kSelNames[std::max(0, std::min(kNumSel - 1, weaponSel_))]);
        if (weaponSel_ == 11) {
            timmyLine_ = "TURRET (" + std::to_string(turretRounds_) + " ROUNDS) - POINT AT THE GROUND, TRIGGER TO PLANT IT";
            timmyLineT_ = 3.0f;
        }
        if (weaponSel_ == 10) {
            timmyLine_ = "REPAIR GUN - AIM AT A DAMAGED BUILDING, HOLD THE TRIGGER";
            timmyLineT_ = 3.0f;
        }
        if (weaponSel_ == 1 && launcherHoming_ > 0) {
            timmyLine_ = "ROCKET LAUNCHER (" + std::to_string(launcherAmmo_) + ") + " + std::to_string(launcherHoming_) + " HOMING - FIRED FIRST";
            timmyLineT_ = 3.0f;
        }
    }
    weaponsTicked_ = true;
    scopeWanted_ = false;
    bowValid_ = bowLinesShown_ = bowDrawing_ = false;  // (a Reaper mission's bow)
    if (weaponSel_ == 2 || weaponSel_ == 3 || weaponSel_ == 8 || weaponSel_ == 9 || (firstPerson_ && heldReaperGun(gh))) {
        // In the bush / holding a bomb or grenade (or a Reaper's gun picked up): no gun in hand (its bolts still fly).
        gunValid_ = false;
        laserOn_ = false;  // (2026-10-28: else the aim laser stayed drawn where it last was - found by a 36 Hz run)
        weapon_.update(dt, false, Vec3{}, Vec3{0, 0, -1});
        if (mgLoop_ && audio_) { audio_->stop(mgLoop_); mgLoop_ = 0; }
        return;
    }  // in the bush / holding a bomb or grenade: no gun in hand
    const bool launcher = weaponSel_ == 1 && launcherModel_;
    const RenderModel* held = heldModel();
    // Handle / muzzle in each model's GAME frame (forward -Y, up +Z): the launcher is a
    // shoulder tube (9.5 units long) held at its pistol grip, a little behind the middle.
    // The hand holds each weapon at its own Ref_handle; shots leave from its Ref_nozzle01 (the
    // weapons' .anm reference nodes: mc_gun.anm, mc_launcher.anm).
    Vec3 handleGame, muzzleGame;
    heldRefs(handleGame, muzzleGame);
    Mat4 ref;
    Vec3 muzzle, fwd;
    if (firstPerson_) {
        // First person: the gun follows the controller's aim pose exactly (shots go where you
        // point); handle at the grip, sized with the body's hands.
        const ControllerState& c = in.hand[gh];
        gunValid_ = held && c.aimPoseValid && c.gripPoseValid;
        if (gunValid_) {
            Pose aim = rig_.toWorld(c.aimPose);
            const Pose grip = rig_.toWorld(c.gripPose);
            // Two hands on a long gun: the support hand's grip near the barrel steadies it - the
            // gun then points from the trigger hand through the support hand.
            const int sh = gh == kRight ? kLeft : kRight;
            const ControllerState& sc = in.hand[sh];
            const bool longGun = weaponSel_ == 1 || gunOfSel(weaponSel_) >= 0;
            if (longGun && sc.gripPoseValid) {
                const Vec3 support = rig_.toWorld(sc.gripPose).position;
                const Vec3 fwd0 = aim.orientation.rotate(Vec3{0, 0, -1});
                const Vec3 front = grip.position + fwd0 * 0.32f * rig_.comfort.worldScale;
                if (!twoHand_ && sc.squeeze > 0.6f && length(support - front) < 0.22f * rig_.comfort.worldScale) {
                    twoHand_ = true;
                    haptics_.push_back({sh, 0.4f, 0.05f});
                    LOGI("Two hands on the gun");
                } else if (twoHand_ && (sc.squeeze < 0.3f || length(support - grip.position) < 0.08f)) {
                    twoHand_ = false;
                }
                if (twoHand_) {
                    const Vec3 f = normalize(support - grip.position);
                    const Vec3 upHint = aim.orientation.rotate(Vec3{0, 1, 0});
                    const Vec3 r = normalize(cross(f, upHint));
                    const Vec3 u = cross(r, f);
                    // The rotation with columns r, u, -f, as a quaternion.
                    const float m00 = r.x, m11 = u.y, m22 = -f.z;
                    const float tr = m00 + m11 + m22;
                    Quat q;
                    if (tr > 0) {
                        const float k2 = std::sqrt(tr + 1.0f) * 2.0f;
                        q = Quat{(u.z - (-f.y)) / k2, ((-f.x) - r.z) / k2, (r.y - u.x) / k2, 0.25f * k2};
                    } else if (m00 > m11 && m00 > m22) {
                        const float k2 = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
                        q = Quat{0.25f * k2, (u.x + r.y) / k2, ((-f.x) + r.z) / k2, (u.z - (-f.y)) / k2};
                    } else if (m11 > m22) {
                        const float k2 = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
                        q = Quat{(u.x + r.y) / k2, 0.25f * k2, ((-f.y) + u.z) / k2, ((-f.x) - r.z) / k2};
                    } else {
                        const float k2 = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
                        q = Quat{((-f.x) + r.z) / k2, ((-f.y) + u.z) / k2, 0.25f * k2, (r.y - u.x) / k2};
                    }
                    aim.orientation = normalize(q);
                }
            } else {
                twoHand_ = false;
            }
            const float bodyScale = eyeModel_.y > 0.1f ? userEye_ / eyeModel_.y : 1.0f;
            const float k = bodyScale * 0.6f;
            const Vec3 handle = giantsToRuntime(handleGame.x, handleGame.y, handleGame.z);
            gunXf_ = Mat4::translation(grip.position) * Mat4::rotation(aim.orientation) * Mat4::scale({k, k, k}) *
                     Mat4::translation(handle * -1.0f);
            gunCounterAt_ = grip.position + aim.orientation.rotate(Vec3{0, 0.085f, 0.05f} * bodyScale);  // (fallback: over the hand)
            gunHandleLocal_ = handle;
            gunCounterValid_ = true;
        }
    } else {
        // Third person: gun in the hand (skeleton's Ref_Rhand position), pointing at the
        // crosshair while aiming, otherwise forward and slightly down; character-sized.
        gunValid_ = held && playerModel_ && playerModel_->nodeWorld(gh == kRight ? "ref_rhand" : "ref_lhand", ref);
        if (gunValid_) {
            const Vec4 hp = bodyXf_ * Vec4{-ref.m[12] * gGameUnitsToMeters, ref.m[14] * gGameUnitsToMeters,
                                           ref.m[13] * gGameUnitsToMeters, 1.0f};
            const Vec3 hand{hp.x, hp.y, hp.z};
            const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
            Vec3 f = tpAiming_ ? normalize(aimTarget_ - hand) : normalize(body.rotate(Vec3{0, -0.5f, -1}));
            // Not aiming: the gun moves with the hand (runs, flies, lands). Its pose relative to the
            // hand bone is taken once from the idle pose (forward and a little down), then follows it.
            const Mat4 hr = gameMatrixToRuntime(ref);
            auto dirOf = [&](const Vec3& v) { const Vec4 r4 = hr * Vec4{v.x, v.y, v.z, 0.0f}; return Vec3{r4.x, r4.y, r4.z}; };
            auto toLocal = [&](const Vec3& v) {  // inverse of the hand's rotation (orthonormal columns)
                const Vec3 cx = dirOf({1, 0, 0}), cy = dirOf({0, 1, 0}), cz = dirOf({0, 0, 1});
                return Vec3{dot(v, cx) / std::max(1e-6f, dot(cx, cx)), dot(v, cy) / std::max(1e-6f, dot(cy, cy)), dot(v, cz) / std::max(1e-6f, dot(cz, cz))};
            };
            if (!tpGunCal_ && player_.anim() == PlayerAnim::Idle) {
                tpGunFwd_ = toLocal(normalize(Vec3{0, -0.5f, -1}));
                tpGunUp_ = toLocal(Vec3{0, 1, 0});
                tpGunCal_ = true;
            }
            Vec3 up{0, 1, 0};
            if (!tpAiming_ && tpGunCal_) {
                f = normalize(body.rotate(dirOf(tpGunFwd_)));
                up = normalize(body.rotate(dirOf(tpGunUp_)));
            }
            // Look rotation: -Z -> f, +Y toward `up`.
            const Quat yawQ = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
            const float pitch = std::asin(std::max(-1.0f, std::min(1.0f, f.y)));
            Quat look = yawQ * Quat::fromAxisAngle({1, 0, 0}, pitch);
            if (!tpAiming_ && tpGunCal_) {
                const Vec3 u0 = look.rotate(Vec3{0, 1, 0});
                const Vec3 side = look.rotate(Vec3{1, 0, 0});
                const float roll = std::atan2(dot(up, side), dot(up, u0));
                look = look * Quat::fromAxisAngle({0, 0, 1}, -roll);
            }
            const float k = 0.75f;  // a bit larger than FP so it reads at chase-camera distance
            const Vec3 handle = giantsToRuntime(handleGame.x, handleGame.y, handleGame.z);
            gunXf_ = Mat4::translation(hand) * Mat4::rotation(look) * Mat4::scale({k, k, k}) * Mat4::translation(handle * -1.0f);
        }
    }
    if (gunValid_) {
        const Vec3 mr = giantsToRuntime(muzzleGame.x, muzzleGame.y, muzzleGame.z);
        const Vec4 m4 = gunXf_ * Vec4{mr.x, mr.y, mr.z, 1.0f};
        const Vec3 fr = giantsToRuntime(kGunForwardGame.x, kGunForwardGame.y, kGunForwardGame.z);
        const Vec4 f4 = gunXf_ * Vec4{fr.x, fr.y, fr.z, 0.0f};
        muzzle = {m4.x, m4.y, m4.z};
        fwd = normalize(Vec3{f4.x, f4.y, f4.z});
        if (!firstPerson_ && tpAiming_) fwd = normalize(aimTarget_ - muzzle);  // shots go to the crosshair
        updateScope(muzzle, fwd, in.headValid ? rig_.toWorld(in.head).position : player_.position);
        // The aim laser's end: what the shot would hit.
        laserOn_ = firstPerson_ && settings_.aimLaser;
        if (laserOn_) {
            float t = 120.0f;
            Vec3 n;
            int tg = -1;
            if (!(weapon_.raycast && weapon_.raycast(muzzle, fwd, 120.0f, t, n, tg))) { t = 120.0f; tg = -1; }
            laserFrom_ = muzzle;
            laserTo_ = muzzle + fwd * t;
            // A Reaper who sees the laser resting on him for a moment dives aside (rework).
            if (tg >= 0 && size_t(tg) < enemies_.enemies.size() && tg == laserTarget_) {
                laserOnT_ += dt;
                Enemy& e = enemies_.enemies[size_t(tg)];
                static const bool passive = std::getenv("GVR_PASSIVE_REAPERS") != nullptr;  // (test targets stand still)
                if (laserOnT_ > 0.45f && e.state != EnemyState::Dead && !e.posted && dodgeCool_ <= 0 && !passive) {
                    const Vec3 side = normalize(cross(fwd, Vec3{0, 1, 0})) * (frand() < 0.5f ? -1.0f : 1.0f);
                    enemies_.push(tg, side * 7.0f);
                    dodgeCool_ = 1.5f;
                    laserOnT_ = 0;
                    LOGI("Reaper %u dodges the laser", e.uid);
                }
            } else {
                laserOnT_ = 0;
            }
            laserTarget_ = tg;
        }
        dodgeCool_ = std::max(0.0f, dodgeCool_ - dt);
    } else {
        laserOn_ = false;
    }
    bool trigger = in.hand[gh].trigger > 0.6f;
    if (turretTrigBlock_) {  // (the pull that planted the turret doesn't also fire what comes back into the hand)
        if (!trigger) turretTrigBlock_ = false;
        trigger = false;
    }
    rocketCooldown_ -= dt;
    if (launcher) {
        // One rocket per pull, 1 s to reload. The gun's bolts still fly (update with no trigger).
        weapon_.update(dt, false, muzzle, fwd);
        if (trigger && !rocketTriggerHeld_ && launcherAmmo_ <= 0 && launcherHoming_ <= 0) {
            sfx2D("me_outam", 0.8f);  // no rockets left
            haptics_.push_back({gh, 0.2f, 0.03f});
        }
        if (trigger && gunValid_ && rocketCooldown_ <= 0 && !rocketTriggerHeld_ && (launcherAmmo_ > 0 || launcherHoming_ > 0)) {
            const bool homing = launcherHoming_ > 0;
            if (homing) --launcherHoming_;
            else --launcherAmmo_;
            Rocket rk{muzzle, fwd * 32.0f, 0.0f};
            rk.kind = homing ? 9 : 0;
            rockets_.push_back(rk);
            rocketCooldown_ = 1.0f;
            haptics_.push_back({gh, 0.9f, 0.12f});
            gunKick(0.30f);
            addFlash(muzzle, {2.2f, 1.4f, 0.6f}, 9.0f, 0.18f);
            sfx("me_msls1", muzzle, 1.0f, 0.95f + 0.1f * frand());
            if (homing) LOGI("Homing rocket fired (%d left)", launcherHoming_);
            else LOGI("Rocket fired (%d left)", launcherAmmo_);
        }
        rocketTriggerHeld_ = trigger;
        return;
    }
    rocketTriggerHeld_ = trigger;
    if (weaponSel_ == 11) {  // the turret in hand: the trigger plants it where the hand points
        weapon_.update(dt, false, muzzle, fwd);
        if (trigger && !turretTrigHeld_ && gunValid_) plantTurret(muzzle, fwd);
        turretTrigHeld_ = trigger;
        return;
    }
    turretTrigHeld_ = trigger;
    if (weaponSel_ == 10) {  // the Repair Gun: a beam, no shots (the gun's bolts already in flight fly on)
        weapon_.update(dt, false, muzzle, fwd);
        updateRepairGun(dt, trigger && gunValid_, muzzle, fwd);
        return;
    }
    repairOn_ = false;
    if (fireGun(dt, trigger, gunValid_, muzzle, fwd)) return;  // shotgun, machine gun, sniper gun
    if (weapon_.update(dt, trigger && gunValid_, muzzle, fwd)) {
        haptics_.push_back({gh, 0.35f, 0.04f});
        gunKick(0.10f);
        addFlash(muzzle, {1.6f, 0.6f, 1.3f}, 5.0f, 0.08f);
        sfx((weapon_.shotsFired() & 1) ? "me_peas1" : "me_peas2", muzzle, 0.7f, 0.95f + 0.1f * frand());
    }
}

// First-person body: scaled so its eyes are at the user's real eye height, placed so its eye
// point sits under the tracked head, feet on the character's ground; arm IK targets from the
// controllers (converted to the model's game-unit frame).
void GameApp::poseFirstPersonBody(const InputFrame& in) {
    // The body at the viewer's size: his eye height over the model's (the giant's view is
    // scaled up, so his body is too - and with his growth).
    const float scale = eyeModel_.y > 0.1f ? userEye_ * rig_.comfort.worldScale / eyeModel_.y : 1.0f;
    const Quat bodyRot = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
    const Vec3 head = in.headValid ? rig_.toWorld(in.head).position : player_.position;
    // Pin the eye point of the CURRENT animated pose (previous frame) under the head, so the
    // body moves back as the run clip leans the torso forward instead of the shoulders
    // swinging into view. Scale uses the upright idle eye height.
    Vec3 base = head - bodyRot.rotate(Vec3{eyeNow_.x, 0, eyeNow_.z} * scale);
    // Vertical pin: the body's eyes are always at the viewer's eyes, so the torso keeps its
    // place under the view (crouching sinks the legs instead of the chest rising into view).
    base.y = std::max(player_.position.y - 1.2f * heroScale_, std::min(player_.position.y + 0.5f * heroScale_, head.y - eyeNow_.y * scale));
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
        // (2026-10-28) Hand presence: the index straight while it is off the trigger (pointing, or along a
        // gun's side), resting bent on it when touching but not pulling.
        a.point = c.touchValid && !c.triggerTouch;
        a.triggerRest = c.touchValid && c.triggerTouch;
        a.thumbUp = c.touchValid && !c.thumbTouch;
    }
}

bool GameApp::hasOriginalAssets() const { return assetWorld_ && assetWorld_->loaded(); }

void GameApp::update(float dt, const InputFrame& in) {
    logFxCost(dt);
    fxCostView_ = true;  // (the first view this frame counts the effects drawn in the views)
    warmedThisFrame_ = false;
    gunCounterValid_ = false;  // (placed again with the gun, if it is in the hand)
    frameDt_ = dt;
    GVR_PROF("update (all)");
    time_ += dt;
    if (dt > 1e-4f) fpsSmooth_ += (1.0f / dt - fpsSmooth_) * std::min(1.0f, dt * 2.0f);
    updateQuality(dt);
    { GVR_PROF("sun"); updateSun(dt); applyWeatherLight(dt); applyUnderwater(dt); }
    updateHeroScale();  // Kabuto's giant scale only in play (not loading, cutscenes, free camera)
    // Story progression: once a mission is complete and its outro has played, the screen fades to
    // the loading / briefing screen (as the original goes through its mission briefing), the next
    // mission loads behind it, and play continues on its own after a few seconds.
    if (missionComplete_ && playMode_ && !cutsceneActive() && assetWorld_ && load_.phase == LoadPhase::Idle && !advanceDone_) {
        advanceDone_ = true;
        const std::string next = assetWorld_->nextMission();
        if (!next.empty()) {
            beginLoading(next, in);
        } else if (std::string world = assetWorld_->nextWorld().empty() ? AssetWorld::nextStoryWorld(worldName_) : assetWorld_->nextWorld();
                   !world.empty()) {
            // The chapter's last mission is done: on to the next world of the story (Story1 -> Story2).
            beginLoading(std::string(), in);
            load_.world = world;
            load_.title = world;
            LOGI("Chapter complete: next world %s", world.c_str());
        }
    }
    // The first mission's briefing, before its opening cutscene (the level is already loaded).
    if (!firstBriefDone_ && std::getenv("GVR_NO_BRIEF")) firstBriefDone_ = true;  // test hook (headless default)
    if (!firstBriefDone_ && !title_ && assetWorld_ && !assetWorld_->missionName().empty() && in.headValid && playMode_) {
        firstBriefDone_ = true;
        beginLoading(assetWorld_->missionName(), in, false);
    }
    if (load_.phase != LoadPhase::Idle) {
        updateLoading(dt, in);
        lastInput_ = in;
        return;
    }
    if (title_) {
        if (!menuOpen_ && menuEdge_.pressed(in.hand[kLeft].menu)) openMenu(in);
        updateTitle(dt, in);
        updateMusic(dt);
        lastInput_ = in;
        return;
    }
    if (needSpawn_ && in.headValid) {
        // A skirmish starts at the player's team's base (facing the way initSkirmish turned him).
        const float sy = skirmish_ ? player_.yaw : assetWorld_->spawnYaw();
        if (!skirmish_ && hero_ != Hero::Kabuto) {
            // An ambient character standing right on the start marker rides on the hero's back
            // (2b_2: "the road home, with someone's wife on your back").
            const Vec3 m = assetWorld_->spawnPoint();
            for (auto& c : chars_)
                if (c.ambient && c.rescue < 0 && !c.boss && length(Vec3{c.pos.x - m.x, 0, c.pos.z - m.z}) < 0.8f) {
                    c.rider = true;
                    LOGI("Character type %d rides on the hero's back", c.type);
                }
        }
        const Vec3 sp = skirmish_ ? sk_.spawnPoint(skLocal_) : clearSpawn(assetWorld_->spawnPoint(), sy);
        rig_.teleportHead(in, sp, sy);
        if (playMode_) {
            teleportPlayer(sp, sy);
            if (!missionHostiles_ && objectives_.empty() && !skirmish_) spawnWave();  // test waves only where the mission has no enemies or goals
            // The mission's opening cutscene (op 0x15), e.g. Baz's lander drop in 1a.
            const MissionData& m = assetWorld_->mission();
            // Test hook (headless): GVR_CUTSCENE=<first shot> plays that cutscene instead; "none" skips it.
            const char* forced = std::getenv("GVR_CUTSCENE");
            if (practice_ || (forced && std::string(forced) == "none")) {}  // (practice: straight in)
            else if (forced && *forced) startCutscene(forced);
            else if (!m.startFlick.empty()) startCutscene(m.startFlick);
        }
        needSpawn_ = false;
    }
    if (cutsceneActive()) {
        updateCutscene(dt, in);
        updateVignette(dt, in);  // (a travelling shot camera closes the comfort ring)
        if (assetWorld_) assetWorld_->update(dt);
        lastInput_ = in;
        return;
    }
    // The left menu button: options (the game pauses while they're open).
    if (menuEdge_.pressed(in.hand[kLeft].menu)) {
        if (menuOpen_) menuOpen_ = false;
        else { pushUnscaledRig(in.head, in.headValid); openMenu(in); popUnscaledRig(); }
    }
    if (menuOpen_) {
        pushUnscaledRig(in.head, in.headValid);  // the menu is at human scale (Kabuto)
        updateMenu(dt, in);
        popUnscaledRig();
        followHead(dt, in);
        if (in.headValid) headWorldRot_ = rig_.toWorld(in.head).orientation;
        lastInput_ = in;
        return;
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
        // X (A when left-handed) in the free camera: skip to the next mission of the world (for
        // testing later missions without replaying the earlier ones); back into play there.
        // X tapped: the next mission. Held 3 s: a new game (forget the checkpoint, back to Story 1's
        // first mission); a buzz at 1 s says the hold registered.
        const bool x = in.hand[mh].primary;
        if (x) {
            const float before = newGameHold_;
            newGameHold_ += dt;
            if (before < 1.0f && newGameHold_ >= 1.0f) haptics_.push_back({mh, 0.3f, 0.05f});
        } else {
            if (xHeld_ && newGameHold_ > 0.0f && newGameHold_ < 1.0f && assetWorld_) {
                std::string next = assetWorld_->nextMission();
                if (next.empty() && !assetWorld_->missions().empty()) next = assetWorld_->missions().front();
                if (!next.empty()) {
                    haptics_.push_back({mh, 0.7f, 0.1f});
                    loadMission(next);
                    setPlayMode(true, in);
                }
            }
            newGameHold_ = 0;
        }
        xHeld_ = x;
        if (newGameHold_ > 3.0f && assetWorld_) {
            newGameHold_ = -1e9f;  // once per hold
            haptics_.push_back({mh, 1.0f, 0.4f});
            LOGI("New game: back to Story1");
            if (!savePath_.empty()) std::remove(savePath_.c_str());
            syringes_ = 0;
            flares_ = 0;
            resetArsenal();
            setPlayMode(true, in);
            if (toLower(worldName_) == "story1" && !assetWorld_->missions().empty()) {
                beginLoading(assetWorld_->missions().front(), in);
            } else {
                beginLoading(std::string(), in);
                load_.world = "Story1";
                load_.title = "Story1";
            }
        }
        rig_.update(dt, in);
    }
    { GVR_PROF("world"); if (assetWorld_) assetWorld_->update(dt); }

    // Haptic confirmation on trigger press (free camera only; in play mode the trigger fires).
    for (int h = 0; h < 2; ++h)
        if (triggerEdge_[h].pressed(in.hand[h].trigger > 0.6f) && !playMode_)
            haptics_.push_back({h, 0.5f, 0.05f});
    // Left thumbstick click toggles the engineering test geometry (free camera only; in play the
    // move stick's click shows the briefing while held).
    if (modeEdge_.pressed(in.hand[kLeft].thumbstickClick) && !playMode_) showTestScene_ = !showTestScene_;
    if (!playMode_ || cutsceneActive()) briefHeld_ = false;
    lastInput_ = in;
}

void GameApp::shutdown() {
    gibs_.clear();  // (cut limbs own meshes: freed while the renderer lives)
    playerModel_.reset();
    assetWorld_.reset();
    renderer_.shutdown();
}

}  // namespace gvr
