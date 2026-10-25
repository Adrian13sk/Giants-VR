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
        loadWorldQuality();
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
            m->setHiddenBones({"head", "neck"});
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
                    const float curl[4] = {std::max(g, tr), std::max(g * 0.9f, 0.25f * tr), g, g};
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
                // Held by its wooden pistol grip (the shaft running down and back to the knob at
                // the rear), not at Ref_handle: that sits on the knuckles modelled over the neck
                // (offline side view of mc_launcher_l0; user report after device test 22).
                launcherHandle_ = {0.0f, 3.85f, -0.45f};
                LOGI("Weapon refs: gun handle (%.2f %.2f %.2f) muzzle (%.2f %.2f %.2f); launcher handle (%.2f %.2f %.2f) muzzle (%.2f %.2f %.2f)",
                     gunHandle_.x, gunHandle_.y, gunHandle_.z, gunMuzzle_.x, gunMuzzle_.y, gunMuzzle_.z, launcherHandle_.x, launcherHandle_.y,
                     launcherHandle_.z, launcherMuzzle_.x, launcherMuzzle_.y, launcherMuzzle_.z);
            }
            launcherModel_ = mc->get("mc_launcher_l0.gbs", "", /*dynamicLit=*/true);
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
                if (creatures_.raycast(o, d, maxD, tc, nc, ic) && (!he || tc < te)) {
                    he = true; te = tc; ne = nc; ie = kCreatureTarget + ic;
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
                    const float tn = -b - std::sqrt(disc);
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
                const bool head = p.y - enemies_.enemies[size_t(target)].pos.y > 1.6f;
                if (head) sfx("bi_hit2", p, 0.8f, 1.4f);
                const bool killed = enemies_.damage(target, (head ? 62.5f : 25.0f) * hm);
                if (killed && enemies_.enemies[size_t(target)].objective >= 0)
                    completeObjective(enemies_.enemies[size_t(target)].objective);
                bleed(p, shotDir, killed, 1.0f);
                if (killed) {
                    blood_.pool(enemies_.enemies[size_t(target)].pos, 1.3f);
                    enemies_.alertNear(enemies_.enemies[size_t(target)].pos, 25.0f);  // a comrade falls
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
                const Vec3 a = player_.position + Vec3{0, 0.35f * hs, 0}, b = player_.position + Vec3{0, 1.65f * hs, 0};
                const bool hp = playMode_ && rayCapsule(o, d, maxD, a, b, player_.radius, tp);
                if (!hw && !hp) return false;
                if (hp && (!hw || tp < tw)) { t = tp; n = d * -1.0f; target = -2; }
                else { t = tw; n = nw; target = -1; }
                return true;
            };
            enemyFire_.onHit = [this](int target, const Vec3& p, const Vec3& dir) {
                if (target == -2) { hitFrom_ = p - dir * 10.0f; hitDirTimer_ = 1.2f; damagePlayer(5.0f, DeathCause::Shot); }
                else if (!shotIntoWater(p, dir)) sfx(frand() < 0.5f ? "bi_hit1" : "bi_dirt1", p, 0.35f, 0.9f + 0.2f * frand());
            };
            // NPCs float in deep water (the sea surface is at 0): they can't walk on the sea floor.
            enemies_.groundHeight = [w](float x, float z) {
                float y;
                return std::max(w->terrain().heightAt(x, z, y) ? y : -20.0f, -1.1f);
            };
            enemies_.collision = &w->collision();
            // Reaper grunt model pool with the original clips (order = EnemyClip).
            const char* eclips[] = {"rpman_stand_scan.anm", "rpman_walk_fire.anm", "rpman_stand_fire.anm",
                                    "rpman_hit_med.anm", "rpman_hit_med2.anm", "rpman_hit_med_die.anm",
                                    "rpman_hit_spin_die.anm", "rpman_hit_gut_die.anm", "rpman_bzka_run.anm",
                                    "rpman_bzka_walk_scan.anm", "rpman_standing_alert_scan.anm", "rpman_hit_blowup.anm",
                                    "rpman_hit_fall_cycle.anm", "rpman_hit_fall_land.anm", "rpman_poke_bush.anm"};
            int poolSize = 6;
            {
                int missionReapers = 0;
                for (const auto& ch : w->characters()) missionReapers += ch.objectClass == 51;
                poolSize = std::min(16, std::max(6, missionReapers));
            }
            for (int k = 0; k < poolSize; ++k) {
                auto em = std::make_unique<SkinnedModel>();
                if (!em->load(w->fs(), *mc, renderer_, "rpman_l0", eclips[0])) break;
                for (int c = 1; c < int(EnemyClip::Count); ++c) em->addAnimation(w->fs(), eclips[c]);
                if (k == 0) {
                    for (int c = 0; c < int(EnemyClip::Count); ++c) enemies_.clipDuration[c] = std::max(0.1f, em->animDuration(c));
                    em->pose(renderer_, 0.0f);
                    Vec3 mn, mx;
                    em->posedBounds(mn, mx);
                    enemyFootOffset_ = -mn.y;  // measured: 1.10 m for rpman_stand_scan
                    LOGI("Reaper foot offset %.2f m", enemyFootOffset_);
                }
                enemyPool_.push_back(std::move(em));
            }
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
    pos.y = firstPerson_ ? player_.position.y : camFloor_;
    // Camera collision: if a cliff or building is between the character and the viewer's eyes,
    // pull the camera in front of it so the character stays visible.
    const float eyeH = ((in && in->headValid) ? in->head.position.y : 1.6f) * ws;
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
            player_.position += qOld.rotate(Vec3{in.head.position.x - headPrev_.x, 0, in.head.position.z - headPrev_.z} * rig_.comfort.worldScale);
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
        return;
    }
    { GVR_PROF("jetski world"); updateJetskiWorld(dt, in); }
    if (riding_) {  // on the Reaperski: its own handling (game_app_jetski.cpp)
        swordValid_ = gunValid_ = false;
        updateRiding(dt, in);
        { GVR_PROF("feel"); updateFeel(dt, in); }
        updateCombat(dt);
        { GVR_PROF("audio"); updateAudio(dt); }
        return;
    }
    PlayerInput pi;
    pi.move = in.hand[mh].thumbstick;
    pi.viewYaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : camYaw_;
    pi.jet = in.hand[th].primary;              // A
    pi.sprint = in.hand[mh].squeeze > 0.5f && !twoHand_;    // move-hand grip (not while it steadies a gun)
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
    updateBush(dt, pi);
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
    else if (hero_ == Hero::Kabuto) { updateKabuto(dt, in); updateThrows(dt, in); updateKabutoPowers(dt, in); updateGlass(dt, in); }
    else { GVR_PROF("weapon"); updateHolsters(dt, in); updateWeapon(dt, in); updateMelee(dt, in); }
    { GVR_PROF("feel"); updateFeel(dt, in); }
    { GVR_PROF("rescue"); updateRescue(dt, in); }
    updateCombat(dt);
    { GVR_PROF("audio"); updateAudio(dt); }
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

void GameApp::sfx(const char* name, const Vec3& pos, float vol, float pitch) {
    if (audio_) audio_->play(name, pos, vol, pitch);
}
void GameApp::sfx2D(const char* name, float vol, float pitch) {
    if (audio_) audio_->play2D(name, vol, pitch);
}

void GameApp::setAudio(AudioMixer* mixer) {
    audio_ = mixer;
    applySettings();  // volumes
    if (!audio_ || !assetWorld_) return;
    static const char* names[] = {
        "me_peas1", "me_peas2", "bi_dirt1", "bi_hit1", "bi_hit2", "ex_bdys1", "rh_sprsh", "rh_fsld1",
        "rpman_hit1", "rpman_hit2", "rpman_hit3", "rpman_hit4", "rpman_die1", "rpman_die2", "rpman_die3",
        "rpman_die4", "rpman_die5", "me_shrt1", "me_shrt2", "me_shrt3", "me_jpkst", "me_jpklp", "me_jpken",
        "me_jpkno", "me_rsgsh", "me_rsgex", "me_sprsh", "me_mchst", "me_mchlp", "me_mchen", "me_outam", "pu_ammo", "me_mtrsh", "me_mtrht", "me_grnth", "me_grnex", "me_minth", "me_minlk", "me_minml", "me_minex", "sr_hbwpl", "sr_hbwsh", "sr_pbwpl", "sr_pbwsh", "sr_stohb", "sr_stopb", "sr_rbwpl", "sr_rbwsh", "sr_rpgpl", "sr_rpgsh", "sr_sbwpl", "sr_sbwsh", "sr_storb", "sr_storp", "sr_stosb", "sr_btosw", "me_fsgr1", "me_fsgr2", "me_hdfl1", "me_hdfl2", "me_sld1", "me_slden",
        // Footsteps by surface, the swimming loop.
        "me_fsgr1", "me_fsgr2", "me_fssa1", "me_fssa2", "me_fsst1", "me_fsst2", "me_fswd1", "me_fswd2", "me_fswa1", "me_fswa2", "me_swmlp",
        // Cleaners eating a body.
        "cl_eatst", "cl_eatlp", "cl_eaten",
        // Rippers: rs_ small, rm_ medium, rl_ large (inferred from the names and the three Ripper types).
        "rs_attk1", "rs_attk2", "rs_chitt", "rs_deat1", "rs_deat2", "rs_feet", "rs_injr1", "rs_injr2",
        "rm_atfr1", "rm_atfr2", "rm_attk1", "rm_chitt", "rm_deat1", "rm_deat2", "rm_feet", "rm_frexp", "rm_inj01",
        "rm_inj02", "rl_atfr1", "rl_atfr2", "rl_attk1", "rl_chitt", "rl_deat1", "rl_deat2", "rl_feet", "rl_frexp1",
        "rl_frexp2", "rl_injr1", "rl_injr2", "ex_bdyl1", "ex_bdyl2",
        // Scenery breaking: trees, stone, wood, metal (large and small).
        "ex_treel", "ex_trees", "ex_stonl", "ex_stons", "ex_woodl", "ex_woods", "ex_metll", "ex_metls", "ex_dirtl", "ex_dirts",
        // Carrying Smarties (1b): pick up / let go / drop off, splash, piranhas.
        "me_smton", "me_smtof", "me_smtdp", "se_splsh", "pi_swrm1", "pi_swrm2",
        // Rocket launcher: launch, explosion.
        "me_msls1", "me_mslex",
        // Delphi (Sea Reaper): sword swish and hits, fire spell (names read as such; provisional).
        "sr_swsh1", "sr_swsh2", "sr_swht1", "sr_swht2", "sr_swht3", "sr_swht4", "sr_spfsh",
        // Ripper nests: a Ripper hatching.
        "rs_spawn", "rm_spawn", "rl_spawn",
        // HUD weapon select, health pickup, injection.
        "ph_selct", "pu_hlth", "me_syrng",
        // Kabuto: footsteps by surface, punches and hits, stomp, landing, eating, roars, death.
        "ka_fsgr1", "ka_fsgr2", "ka_fssa1", "ka_fssa2", "ka_fsst1", "ka_fsst2", "ka_fswa1", "ka_fswa2", "ka_pnch1", "ka_pnch2",
        "ka_ltht1", "ka_mdht1", "ka_hvht1", "ka_stomp", "ka_lndhd", "ka_grab", "ka_eat1", "ka_eat2", "ka_belch", "ka_sniff",
        "ka_roar1", "ka_roar2", "ka_roar3", "ka_roar4", "ka_deth1", "ka_deth2", "ka_wound", "ka_smrt1", "ka_smrt2",
        // Vimps and their meat.
        "vi_deat1", "vi_deat2", "vi_injr1", "vi_injr2", "vi_moo1", "vi_moo2", "vi_squi1", "pu_meat1", "pu_meat2", "ka_vimp1", "ka_vimp2",
        // The Reaperski: engine, rider on/off, gates, bumps, turbo, start / win / lose.
        "js_ron", "js_roff", "js_ridl1", "js_rmve1", "js_rrvu1", "js_rrvd1", "js_buoy", "js_mbuoy", "js_bump1", "js_bump2", "js_tbact",
        "js_start", "js_finis", "js_win", "js_lose",
        // Offspring and Charger (bosses).
        "of_claw1", "of_claw2", "of_roar1", "of_roar2", "of_dhit1", "of_dhit2", "of_deth1", "of_deth2", "ca_snort", "ca_roar", "ca_injr1",
        "ca_injr2", "ca_deat1", "ca_deat2",
        // Sonak.
        "so_atst1", "so_atsol", "so_snrt1", "so_roar1", "so_roar2", "so_injr1", "so_injr2", "so_deat1", "so_deat2", "pr_wsmil"};
    std::vector<std::string> all(std::begin(names), std::end(names));
    for (int k = 1; k <= 15; ++k)  // Smartie voice pairs gsmNN_1 / gsmNN_2 (no 07 in the data)
        for (int j = 1; j <= 2; ++j) {
            if (k == 7) continue;
            char n[16];
            std::snprintf(n, sizeof n, "gsm%02d_%d", k, j);
            all.push_back(n);
        }
    size_t ok = 0;
    for (const std::string& ns : all) {
        const char* n = ns.c_str();
        std::vector<uint8_t> bytes;
        WavData w;
        std::string err;
        if (assetWorld_->fs().read(std::string(n) + ".wav", bytes) && parseWav(bytes.data(), bytes.size(), w, &err) &&
            audio_->addSound(n, std::move(w)))
            ++ok;
        else
            LOGW("Sound %s unavailable %s", n, err.c_str());
    }
    LOGI("Audio: %zu/%zu original sounds loaded", ok, all.size());
    initMusic();
}

void GameApp::updateAudio(float dt) {
    updateMusic(dt);
    if (!audio_) return;
    hurtCooldown_ -= dt;
    audio_->setListener(rig_.toWorld(lastInput_.head).position, headWorldRot_);
    if (!playMode_) {
        if (jetLoop_) { audio_->stop(jetLoop_); jetLoop_ = 0; }
        if (slideLoop_) { audio_->stop(slideLoop_); slideLoop_ = 0; }
        if (swimLoop_) { audio_->stop(swimLoop_); swimLoop_ = 0; }
        return;
    }
    // Jetpack: start, loop while thrusting, end; a sputter when the tank runs dry.
    const bool thr = player_.thrusting();
    if (thr && !wasThrusting_) {
        sfx2D("me_jpkst", 0.6f);
        jetLoop_ = audio_->play2D("me_jpklp", 0.45f, 1.0f, true);
    }
    if (!thr && wasThrusting_) {
        if (jetLoop_) { audio_->stop(jetLoop_); jetLoop_ = 0; }
        sfx2D(player_.fuel() <= 0.0f ? "me_jpkno" : "me_jpken", 0.6f);
    }
    wasThrusting_ = thr;
    // Slide loop on steep ground.
    const bool sl = player_.sliding();
    if (sl && !wasSliding_) slideLoop_ = audio_->play2D("me_sld1", 0.5f, 1.0f, true);
    if (!sl && wasSliding_) {
        if (slideLoop_) { audio_->stop(slideLoop_); slideLoop_ = 0; }
        sfx2D("me_slden", 0.5f);
    }
    wasSliding_ = sl;
    // Landing after a real fall.
    if (!player_.onGround) {
        airTime_ += dt;
        fallSpeed_ = std::max(fallSpeed_, -player_.velocity.y);
    } else {
        if (!wasOnGround_ && airTime_ > 0.3f) sfx2D(fallSpeed_ > 6.0f ? "me_hdfl1" : "me_hdfl2", fallSpeed_ > 6.0f ? 0.8f : 0.45f);
        airTime_ = 0;
        fallSpeed_ = 0;
    }
    wasOnGround_ = player_.onGround;
    // Footsteps on the run/strafe cycle (two per cycle).
    const PlayerAnim pa = player_.anim();
    if (player_.onGround && (pa == PlayerAnim::Run || pa == PlayerAnim::Strafe || pa == PlayerAnim::Turbo) && playerModel_) {
        const float dur = std::max(0.1f, playerModel_->animDuration(clip_[int(pa)]));
        // Half-cycle counter: a footstep each time the clip enters a new half (either direction).
        const float halfIndex = std::floor(player_.animTime() / dur * 2.0f);
        if (halfIndex != prevRunPhase_) {
            // The original footstep sets by surface (me_fs + gr grass, sa sand, st stone, wd wood,
            // wa water): water when wading, wood on structures (bridges, walkways), sand on the
            // beach just above the sea, grass elsewhere. Louder than before (device test 19).
            const bool wade = player_.position.y < player_.seaLevel - 0.05f;
            const bool giant = hero_ == Hero::Kabuto;
            const char* set = wade ? "wa" : (player_.onObject() ? (giant ? "st" : "wd") : (player_.position.y < player_.seaLevel + 1.5f ? "sa" : "gr"));
            const std::string n = std::string(giant ? "ka_fs" : "me_fs") + set + ((int(halfIndex) & 1) ? "2" : "1");
            sfx2D(n.c_str(), wade ? 0.9f : (giant ? 1.0f : 0.75f), 0.95f + 0.1f * frand());
            if (giant) {
                // Every step of the giant: a thud in both hands and dust at the foot.
                haptics_.push_back({kLeft, 0.35f, 0.08f});
                haptics_.push_back({kRight, 0.35f, 0.08f});
                const Quat r = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
                const Vec3 foot = player_.position + r.rotate(Vec3{((int(halfIndex) & 1) ? 0.12f : -0.12f) * player_.height, 0, 0});
                if (!wade) dustBurst(foot, 0.08f * player_.height, 4);
            }
            if (wade) {  // a splash at the foot
                const Quat r = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
                const Vec3 foot = player_.position + r.rotate(Vec3{(int(halfIndex) & 1) ? 0.2f : -0.2f, 0, -0.2f});
                waterFx_.push_back({Vec3{foot.x, 0.02f, foot.z}, 0.0f, 0, 0.35f});
            }
        }
        prevRunPhase_ = halfIndex;
    }
    // Swimming: the stroke loop while moving in deep water.
    const bool strokes = player_.swimming() && player_.horizontalSpeed() > 0.3f;
    if (strokes && !swimLoop_) swimLoop_ = audio_->play2D("me_swmlp", 0.6f, 1.0f, true);
    if (!strokes && swimLoop_) { audio_->stop(swimLoop_); swimLoop_ = 0; }
    // Reaper footsteps while running.
    if (slotStep_.size() != enemyPool_.size()) slotStep_.assign(enemyPool_.size(), 0.0f);
    for (const auto& e : enemies_.enemies) {
        if (e.slot < 0 || e.clip != EnemyClip::Run || e.state == EnemyState::Dead) continue;
        float& t = slotStep_[size_t(e.slot)];
        t -= dt;
        if (t <= 0) {
            t = 0.3f;
            if (length(e.pos - player_.position) < 30.0f) sfx("rh_fsld1", e.pos, 0.75f, 0.9f + 0.2f * frand());
        }
    }
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
            // Most Rippers start underground (user report on the original: nine in ten), under a
            // mound, and come up when they feel the player or a shot lands near (GVR_BURIED sets
            // the share for tests).
            static const float buried = std::getenv("GVR_BURIED") ? float(std::atof(std::getenv("GVR_BURIED"))) : 0.9f;
            const bool besieging = length(Vec3{ch.position.x - timmyAt.x, 0, ch.position.z - timmyAt.z}) < 18.0f;
            if (frand() < buried && !besieging) creatures_.bury(idx);
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
            LOGI("Objective: kill %s (%.0f hp, %.1f m tall%s)", ch.model.c_str(), ci.hp, ci.bossH, kind ? ", fights" : "");
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
    if (!nests_.empty()) {
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
        const int pool = std::min(12, 3 * int(nests_.size()));
        const int bigPool = bigNests == 0 ? 0 : (bigNests == int(nests_.size()) ? pool : std::max(3, pool * bigNests / int(nests_.size())));
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
    initDestruction(w, mc);
    LOGI("Mission characters: %zu Rippers, %zu Reapers, %zu ambient", rippers, reapers, ambient);
    // The VR map's island model is built now, behind the loading screen (it took ~5 ms on the host
    // when built on the first frame of play).
    if (worldName_ != mapWorld_) mapReset();
    buildMapMesh();
}

void GameApp::spawnGore(const Vec3& pos, const Vec3& dir, float scale) {
    // The original gore pieces (rip_gore_head, rip_gore_arm x2) thrown along the killing shot.
    blood_.burst(pos + Vec3{0, 0.5f * scale, 0}, dir + Vec3{0, 0.8f, 0}, 28, 5.5f, 0.08f * std::max(0.7f, scale));
    blood_.pool(pos, 1.2f * std::max(0.7f, scale));
    const RenderModel* parts[3] = {goreHead_, goreArm_, goreArm_};
    Vec3 push{dir.x, 0, dir.z};
    if (length(push) > 1e-3f) push = normalize(push);
    for (const RenderModel* m : parts) {
        if (!m) continue;
        Gib g;
        g.model = m;
        g.pos = pos + Vec3{0, 0.5f * scale, 0};
        g.vel = push * (3.0f + 2.0f * frand()) + Vec3{(frand() - 0.5f) * 4.0f, 4.0f + 3.0f * frand(), (frand() - 0.5f) * 4.0f};
        g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
        g.angle = frand() * 6.28f;
        g.spin = 4.0f + 6.0f * frand();
        g.scale = scale * 0.8f;
        g.life = 0;
        gibs_.push_back(g);
    }
}

void GameApp::creatureSound(const CreatureEvent& e) {
    // Sound sets by Ripper size: rs_ small, rm_ medium, rl_ large (meanings inferred from names).
    const int k = int(e.size);
    const char* pre = k == 0 ? "rs_" : (k == 1 ? "rm_" : "rl_");
    const float pitch = 0.92f + 0.16f * frand();
    auto play = [&](const char* a, const char* b, float vol) {
        const std::string n = std::string(pre) + ((b && frand() < 0.5f) ? b : a);
        sfx(n.c_str(), e.pos, vol, pitch);
    };
    switch (e.kind) {
        case CreatureEvent::Alert: play("chitt", nullptr, 0.8f); break;
        case CreatureEvent::Swing:
        case CreatureEvent::Lunge:
            if (k == 0) play("attk1", "attk2", 0.9f); else play("attk1", nullptr, 0.9f);
            break;
        case CreatureEvent::SpitLaunch: play("atfr1", "atfr2", 1.0f); break;
        case CreatureEvent::SpitBurst:
            if (k == 2) play("frexp1", "frexp2", 1.0f); else sfx("rm_frexp", e.pos, 1.0f, pitch);
            break;
        case CreatureEvent::Strike: break;  // the player's hurt sound covers it
        case CreatureEvent::Hurt:
            if (k == 1) play("inj01", "inj02", 0.9f); else play("injr1", "injr2", 0.9f);
            break;
        case CreatureEvent::Die:
            ++recentKills_;
            play("deat1", "deat2", 1.0f);
            if (e.burst) sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", e.pos, 0.9f, 0.95f + 0.1f * frand());
            break;
        default: break;  // burrowing and latching: burrowEvent
    }
}

// ---------------------------------------------------------------------------------------------
// Blood, corpses, Cleaners

void GameApp::initGore(AssetWorld* w, ModelCache* mc) {
    if (!goreQuad_) {
        MeshData q;
        const float c[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        for (const auto& p : c) {
            Vertex v{};
            v.pos[0] = p[0];
            v.pos[1] = p[1];
            v.normal[2] = 1;
            v.uv[0] = p[0] + 0.5f;
            v.uv[1] = 0.5f - p[1];
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
            q.vertices.push_back(v);
        }
        q.indices = {0, 1, 2, 0, 2, 3};
        goreQuad_ = renderer_.uploadMesh(q);
        bloodTex_[0] = mc->texture("efx_splat");
        bloodTex_[1] = mc->texture("efx_splat2");
        bloodTex_[2] = mc->texture("bloodblob");  // soft round: pools and clouds in the water
        sprayTex_ = mc->texture("efx_spray");
        dropTex_ = mc->texture("efx_blood");
        const char* rc[4] = {"rpman_chunk_head.gbs", "rpman_chunk_body.gbs", "rpman_chunk_main.gbs", "rpman_chunk_hand.gbs"};
        for (int k = 0; k < 4; ++k) reaperChunk_[k] = mc->get(rc[k], "", /*dynamicLit=*/true);
        for (int k = 0; k < 6; ++k) meccChunk_[k] = mc->get("MC_chunk" + std::to_string(k + 1) + ".gbs", "", true);
        for (int k = 0; k < 18; ++k) {  // three swarms at a time
            auto m = std::make_unique<SkinnedModel>();
            if (!m->load(w->fs(), *mc, renderer_, "cleaner_l0", "cleaner_walk.anm")) break;
            m->addAnimation(w->fs(), "cleaner_eat.anm");
            m->addAnimation(w->fs(), "cleaner_jump.anm");
            cleanerPool_.push_back(std::move(m));
        }
        LOGI("Gore: blood textures %u %u %u, %zu Cleaners, Reaper chunks %d", bloodTex_[0], bloodTex_[1], bloodTex_[2],
             cleanerPool_.size(), reaperChunk_[0] != nullptr);
    }
    auto ground = [w](float x, float z) {
        float y;
        return w->terrain().heightAt(x, z, y) ? y : -20.0f;
    };
    blood_ = BloodSystem{};
    blood_.groundHeight = ground;
    blood_.seaLevel = 0.0f;
    cleaners_.cleaners.clear();
    cleaners_.groundHeight = [w](float x, float z) {
        float y;
        return std::max(w->terrain().heightAt(x, z, y) ? y : -20.0f, -0.3f);
    };
    enemies_.params.corpsesPersist = true;
    creatures_.params.corpsesPersist = true;
}

void GameApp::bloodInWater(const Vec3& at, float size) {
    blood_.pool(Vec3{at.x + (frand() - 0.5f) * 0.8f, -0.5f, at.z + (frand() - 0.5f) * 0.8f}, size);
}

void GameApp::bleed(const Vec3& at, const Vec3& dir, bool kill, float scale) {
    // The blood comes out of the far side of a wound and sprays up a little.
    blood_.burst(at, dir + Vec3{0, 0.4f, 0}, kill ? 16 : 6, kill ? 4.0f : 3.0f, 0.07f * std::max(0.6f, scale));
}

void GameApp::gibCorpse(const Vec3& pos, float yaw, const Vec3& dir, bool reaper, float scale) {
    // Either bursts (pieces flung far, a big spray) or falls apart (pieces tumble close by).
    const bool burst = frand() < 0.5f;
    const float k = burst ? 1.0f : 0.35f;
    const RenderModel* reap[4] = {reaperChunk_[0], reaperChunk_[1], reaperChunk_[2], reaperChunk_[3]};
    const RenderModel* rip[3] = {goreHead_, goreArm_, goreArm_};
    const int n = reaper ? 4 : 3;
    Vec3 push{dir.x, 0, dir.z};
    if (length(push) > 1e-3f) push = normalize(push);
    for (int i = 0; i < n; ++i) {
        const RenderModel* m = reaper ? reap[i] : rip[i];
        if (!m) continue;
        Gib g;
        g.model = m;
        const Vec3 along = Quat::fromAxisAngle({0, 1, 0}, yaw).rotate(Vec3{0, 0, (float(i) - 1.5f) * 0.35f * scale});
        g.pos = pos + along + Vec3{0, 0.3f * scale, 0};
        g.vel = (push * (3.0f + 2.0f * frand()) + Vec3{(frand() - 0.5f) * 4.0f, 3.0f + 3.0f * frand(), (frand() - 0.5f) * 4.0f}) * k;
        g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
        g.angle = frand() * 6.28f;
        g.spin = (4.0f + 6.0f * frand()) * k;
        g.scale = reaper ? 1.0f : scale * 0.8f;
        g.life = 0;
        gibs_.push_back(g);
        blood_.burst(g.pos, g.vel + Vec3{0, 1, 0}, 4, 2.5f, 0.07f);
    }
    blood_.burst(pos + Vec3{0, 0.4f * scale, 0}, Vec3{0, 1, 0} + push, burst ? 30 : 14, burst ? 6.0f : 3.0f, 0.08f);
    blood_.pool(pos, 1.6f * std::max(0.7f, scale));
    sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", pos, 1.0f, 0.9f + 0.2f * frand());
    LOGI("A body %s", burst ? "bursts" : "falls apart");
}

bool GameApp::corpseRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& target) const {
    // Bodies lying on the ground: the box of the posed body (its death pose) in the body's frame.
    bool hit = false;
    float best = maxD;
    auto test = [&](const Vec3& pos, float yaw, const Vec3& mn, const Vec3& mx, int id) {
        const Quat inv = Quat::fromAxisAngle({0, 1, 0}, -yaw);
        const Vec3 lo = inv.rotate(o - pos), ld = inv.rotate(d);
        float t0 = 0, t1 = best;
        const float ro[3] = {lo.x, lo.y, lo.z}, rd[3] = {ld.x, ld.y, ld.z}, bmn[3] = {mn.x, mn.y, mn.z}, bmx[3] = {mx.x, mx.y, mx.z};
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(rd[a]) < 1e-6f) {
                if (ro[a] < bmn[a] || ro[a] > bmx[a]) return;
                continue;
            }
            float ta = (bmn[a] - ro[a]) / rd[a], tb = (bmx[a] - ro[a]) / rd[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return;
        }
        if (t0 < best) { best = t0; target = id; hit = true; }
    };
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Enemy& e = enemies_.enemies[i];
        if (e.state != EnemyState::Dead || e.stateTime < 0.6f || e.slot < 0) continue;
        Vec3 mn, mx;
        enemyPool_[size_t(e.slot)]->posedBounds(mn, mx);
        const float lift = slotLift_[size_t(e.slot)];
        test(e.pos + Vec3{0, lift, 0}, e.yaw, mn, mx, kCorpseEnemy + int(i));
    }
    for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
        const Creature& c = creatures_.creatures[i];
        if (c.state != CreatureState::Dead || c.exploded || c.stateTime < 0.6f || c.slot < 0 || size_t(c.slot) >= chars_.size()) continue;
        const CharacterInst& ci = chars_[size_t(c.slot)];
        Vec3 mn, mx;
        ci.model->posedBounds(mn, mx);
        test(c.pos + Vec3{0, ci.lift, 0}, c.yaw, mn * ci.scale, mx * ci.scale, kCorpseCreature + int(i));
    }
    if (hit) t = best;
    return hit;
}

void GameApp::shootCorpse(int target, const Vec3& p, const Vec3& dir) {
    if (target >= kCorpseCreature) {
        Creature& c = creatures_.creatures[size_t(target - kCorpseCreature)];
        bleed(p, dir, false, c.scale);
        sfx("ex_bdys1", p, 0.5f, 1.1f + 0.2f * frand());
        c.corpseHp -= 25.0f;
        if (c.corpseHp <= 0 && !c.gibbed) {
            c.gibbed = true;
            gibCorpse(c.pos, c.yaw, dir, false, c.scale);
        }
        return;
    }
    Enemy& e = enemies_.enemies[size_t(target - kCorpseEnemy)];
    bleed(p, dir, false, 1.0f);
    sfx("ex_bdys1", p, 0.5f, 1.0f + 0.2f * frand());
    e.corpseHp -= 25.0f;
    if (e.corpseHp <= 0 && !e.gibbed) {
        e.gibbed = true;
        gibCorpse(e.pos, e.yaw, dir, true, 1.0f);
    }
}

void GameApp::updateGore(float dt) {
    blood_.update(dt);
    // Bodies lying a while attract the Cleaners (within sight of the player; far ones just lie).
    auto consider = [&](uint32_t uid, const Vec3& pos, float age, bool& called, float size) {
        if (called || age < 14.0f || length(pos - player_.position) > 60.0f || cleanerPool_.empty()) return;
        float h;
        if (pos.y < -0.4f || !assetWorld_->terrain().heightAt(pos.x, pos.z, h)) return;  // in the sea: the piranhas' (below)
        // A swarm: five or six to a Reaper, three or four to a small Ripper (device test 20: more).
        const size_t busy = cleaners_.cleaners.size();
        const int want = std::min(int(cleanerPool_.size() - std::min(busy, cleanerPool_.size())), (size > 0.8f ? 5 : 3) + int(frand() * 1.99f));
        if (want < 2) return;
        called = true;
        cleaners_.call(uid, pos, want, frand() * 6.28f);
        LOGI("Cleaners called to body %u", uid);
    };
    for (auto& e : enemies_.enemies)
        if (e.state == EnemyState::Dead && !e.gibbed) consider(e.uid, e.pos, e.stateTime, e.cleanersCalled, 1.0f);
    for (auto& c : creatures_.creatures)
        if (c.state == CreatureState::Dead && !c.exploded && !c.gibbed) consider(0x80000000u | c.uid, c.pos, c.stateTime, c.cleanersCalled, c.scale);
    // Bodies in deep water: the piranhas take them (blood in the water, gone in a few seconds).
    for (auto& e : enemies_.enemies)
        if (e.state == EnemyState::Dead && !e.gibbed && e.pos.y < -0.4f && e.stateTime > 1.0f) {
            e.eaten += dt / 6.0f;
            if (frand() < dt * 2.0f) bloodInWater(e.pos, 0.7f);
        }
    for (auto& c : creatures_.creatures)
        if (c.state == CreatureState::Dead && !c.exploded && !c.gibbed && c.pos.y < -0.4f && c.stateTime > 1.0f) {
            c.eaten += dt / 5.0f;
            if (frand() < dt * 2.0f) bloodInWater(c.pos, 0.5f);
        }
    auto bodyAt = [this](uint32_t id, Vec3& at) {
        if (id & 0x80000000u) {
            for (const auto& c : creatures_.creatures)
                if (c.uid == (id & 0x7fffffffu)) {
                    if (c.state != CreatureState::Dead || c.gibbed || c.eaten >= 1.0f) return false;
                    at = c.pos;
                    return true;
                }
            return false;
        }
        for (const auto& e : enemies_.enemies)
            if (e.uid == id) {
                if (e.state != EnemyState::Dead || e.gibbed || e.eaten >= 1.0f) return false;
                at = e.pos;
                return true;
            }
        return false;
    };
    eatenNow_.clear();
    cleanerEvents_.clear();
    cleaners_.update(dt, bodyAt, eatenNow_, cleanerEvents_);
    for (const auto& pr : eatenNow_) {
        if (pr.first & 0x80000000u) {
            for (auto& c : creatures_.creatures)
                if (c.uid == (pr.first & 0x7fffffffu)) c.eaten = std::min(1.0f, c.eaten + pr.second * (1.6f / std::max(0.5f, c.scale)));
        } else {
            for (auto& e : enemies_.enemies)
                if (e.uid == pr.first) {
                    e.eaten = std::min(1.0f, e.eaten + pr.second);
                    if (e.eaten >= 1.0f) LOGI("Reaper body %u eaten by the Cleaners", e.uid);
                }
        }
    }
    for (const auto& ev : cleanerEvents_) {
        if (ev.kind == CleanerEvent::StartEat) {
            LOGI("Cleaners eating body %u", ev.target);
            sfx("cl_eatst", ev.pos, 0.9f);
            if (audio_) eatLoops_.push_back({ev.target, audio_->play("cl_eatlp", ev.pos, 0.7f, 1.0f, true)});
        }
    }
    // Eating finished (or the body went another way): stop the loop, a last gulp.
    for (auto it = eatLoops_.begin(); it != eatLoops_.end();) {
        Vec3 at;
        if (bodyAt(it->first, at)) { ++it; continue; }
        if (audio_) audio_->stop(it->second);
        LOGI("Body %u eaten", it->first);
        for (const auto& c : cleaners_.cleaners)
            if (c.target == it->first) { sfx("cl_eaten", c.pos, 0.9f); break; }
        it = eatLoops_.erase(it);
    }
    // Cleaner models: walk / eat / (jump while burrowing away).
    for (size_t k = 0; k < cleaners_.cleaners.size() && k < cleanerPool_.size(); ++k) {
        const Cleaner& c = cleaners_.cleaners[k];
        if (length(c.pos - player_.position) > 80.0f) continue;
        const int clip = c.state == CleanerState::Eat ? 1 : (c.sink > 0 ? 2 : 0);
        cleanerPool_[k]->poseBlend(renderer_, clip, c.anim * (clip == 0 ? 1.6f : 1.0f), clip, c.anim, 0.0f);
    }
}

void GameApp::renderGore() {
    // Blood splats, drops and sprays are drawn in batches (renderFx); the Cleaners here.
    const Vec3 eye = renderer_.currentView().eyePosition;
    for (size_t k = 0; k < cleaners_.cleaners.size() && k < cleanerPool_.size(); ++k) {
        const Cleaner& c = cleaners_.cleaners[k];
        if (length(c.pos - eye) > 80.0f) continue;
        cleanerPool_[k]->render(renderer_, Mat4::translation(c.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, c.yaw)));
    }
}

void GameApp::updateCharacters(float dt) {
    GVR_PROF("chars (all)");
    {
        GVR_PROF("creatures ai");
        static const bool passive = std::getenv("GVR_PASSIVE_CREATURES") != nullptr;  // test hook: Rippers ignore the player
        creatures_.update(dt, player_.position, playMode_ && playerHealth_ > 0 && !dying_ && !passive, creatureEvents_);
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
        else ci.lift += (-mn.y * ci.scale - ci.lift) * std::min(1.0f, dt / 0.06f);
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
        g.life += dt;
        g.vel.y -= 14.0f * dt;
        g.pos += g.vel * dt;
        g.angle += g.spin * dt;
        const float ground = enemies_.groundHeight ? enemies_.groundHeight(g.pos.x, g.pos.z) : 0.0f;
        if (g.life > 18.0f) {  // sink away
            g.pos.y -= 0.4f * dt;
            continue;
        }
        if (g.pos.y < ground + 0.1f * g.scale) {  // bounce, then settle
            g.pos.y = ground + 0.1f * g.scale;
            g.vel = Vec3{g.vel.x * 0.5f, std::fabs(g.vel.y) * 0.3f, g.vel.z * 0.5f};
            g.spin *= 0.5f;
        }
    }
    gibs_.erase(std::remove_if(gibs_.begin(), gibs_.end(), [](const Gib& g) { return g.life > 20.0f; }), gibs_.end());
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

void GameApp::initMission() {
    // Proximity triggers on placed objects (death triggers were added with their characters).
    for (const auto& t : assetWorld_->triggerObjects())
        if (t.kind == 4) {  // "On Vimp Meat Proximity": bring the meat here
            objectives_.push_back({4, t.boundsMin, t.boundsMax});
            LOGI("Objective: bring %d pieces of Vimp meat to type %d", kVimpMeatNeeded, int(t.type));
        }
    if (assetWorld_->startObject_ >= 0)
        for (const auto& o : assetWorld_->mission().objects)
            if (o.group == 3 && o.parent < 0 && toLower(assetWorld_->registry().modelName(o.type)) == "marker") {
                Vec3 p = giantsToRuntime(o.x, o.y, o.z);
                float gy;
                if (assetWorld_->terrain().heightAt(p.x, p.z, gy)) p.y = gy;
                spawnRivalKabuto(p, o.yaw * kPi / 180.0f);
                break;
            }
    // Death triggers on buildings: destroy it. The trigger is matched to the destructible of the
    // same type nearest its centre.
    for (const auto& t : assetWorld_->triggerObjects()) {
        if (t.kind != 2) continue;
        const Vec3 c = (t.boundsMin + t.boundsMax) * 0.5f;
        // Class 97: a group spawner ("1mcsonak_4guards", "1tsonak_4guards", "1tsonak"): a Sonak - the
        // Reapers' war beast (sonak_l0, 350 hp) - and its Reaper guards. Ours: the Sonak stands at
        // the spawner with the guards round it; the group dies with all of them (a lone Sonak
        // spawner gets one guard).
        if (const ObjectType* ot = assetWorld_->registry().type(t.type); ot && ot->objectClass == 97) {
            const std::string nm = assetWorld_->registry().modelName(t.type);
            const int guards = nm.find("4guards") != std::string::npos ? 4 : 1;
            for (int g = 0; g < guards; ++g) {
                const float a = 6.2832f * float(g) / float(guards);
                Vec3 p = c + Vec3{std::cos(a) * 4.0f, 0, std::sin(a) * 4.0f};
                float gy;
                if (assetWorld_->terrain().heightAt(p.x, p.z, gy)) p.y = gy;
                const int ei = enemies_.spawn(p, a);
                enemies_.enemies[size_t(ei)].objective = int(objectives_.size());
                objectives_.push_back({2, p, p});
            }
            {
                const ObjectType* st = assetWorld_->registry().type(575);  // sonak_l0
                Vec3 sp = c;
                float sy;
                if (assetWorld_->terrain().heightAt(sp.x, sp.z, sy)) sp.y = sy;
                spawnBoss(4, "sonak_l0", "sonak_idle.anm", sp, frand() * 6.28f, st && st->hitPoints > 0 ? st->hitPoints : 350.0f);
            }
            missionHostiles_ = true;
            LOGI("Objective: kill the %d guard%s of %s at game (%.0f, %.0f)", guards, guards > 1 ? "s" : "", nm.c_str(), -c.x / gGameUnitsToMeters,
                 c.z / gGameUnitsToMeters);
            continue;
        }
        int best = -1;
        float bestD = 12.0f;
        const auto& objs = assetWorld_->worldObjects();
        for (size_t k = 0; k < destruct_.items.size(); ++k) {
            const auto& o = objs[size_t(destruct_.items[k].object)];
            const float d = length(Vec3{o.centre.x - c.x, 0, o.centre.z - c.z});
            if (o.type == t.type && d < bestD) { bestD = d; best = int(k); }
        }
        if (best < 0) { LOGW("Death trigger on type %d: no destructible found", int(t.type)); continue; }
        Objective ob{5, t.boundsMin, t.boundsMax};
        ob.item = best;
        objectives_.push_back(ob);
        LOGI("Objective: destroy %s at game (%.0f, %.0f)", objs[size_t(destruct_.items[size_t(best)].object)].name.c_str(),
             -c.x / gGameUnitsToMeters, c.z / gGameUnitsToMeters);
    }
    for (const auto& t : assetWorld_->triggerObjects())
        if (t.kind == 1) {
            objectives_.push_back({1, t.boundsMin, t.boundsMax});
            const Vec3 c = (t.boundsMin + t.boundsMax) * 0.5f;
            LOGI("Objective: reach type %d at game (%.0f, %.0f), %.1f m wide", int(t.type), -c.x / gGameUnitsToMeters,
                 c.z / gGameUnitsToMeters, t.boundsMax.x - t.boundsMin.x);
        }
    // (Reaperski riding and races are set up at the end, once the hero is known.)
    // Starting inventory: a Mecc mission lists the items the player is handed (op 0x0E).
    // weapondefs.bin's item table names item 1 the handgun (IconHandgun, object mc_gun_l0), not
    // the jetpack as first read; the jetpack has no item there. We still gate the jetpack on it in
    // Story 1 (absent only in 1a_1_timmy, where Baz loses his kit in the opening cutscene): kept
    // because it plays as before, now known to be our rule rather than the data's.
    const MissionData& m = assetWorld_->mission();
    bool jet = true;
    if (m.setup[0] == 2) jet = std::find(m.items.begin(), m.items.end(), 1u) != m.items.end();
    // Only Story 1 starts without it (Baz loses it in 1a); later chapters' item lists read
    // differently, and he keeps it from then on (2a lists no item 1 either).
    if (!jet && toLower(worldName_) != "story1") jet = true;
    player_.jetpackAvailable = jet;
    // The items he is handed (weapondefs.bin's item table: 3 shotgun, 5 machine gun, 6 sniper gun,
    // 14 rocket launcher; game_app_arsenal.cpp). Item 3 was read as the launcher until the table was
    // decoded; 1c-2c place mc_pickup_shells, the shotgun's ammo, and Timmy's present at the end of 1b
    // (flk_story1_get_rpg*) is animated with "shotgun" / "Ref_shotgun" nodes.
    grantMissionItems(m.items, "mission start");  // (Reaper missions: her bows, items 44-46)
    if (std::getenv("GVR_LAUNCHER") && launcherModel_) { hasLauncher_ = true; launcherAmmo_ = launcherMax_; }  // test hook
    // The world decides the hero (Reaper story: Delphi); she has no jetpack.
    // The mission's race (setup op 0x0B: 1 Kabuto, 2 Mecc, 3 Reaper) picks the hero - kstory5_5 is
    // played as the Meccaryn; the world name decides where the data gives none.
    const uint32_t race = m.setup[0];
    setHero(race == 1 ? Hero::Kabuto : (race == 2 ? Hero::Mecc : (race == 3 ? Hero::Delphi : heroForWorld(worldName_))));
    if (hero_ != Hero::Mecc) player_.jetpackAvailable = false;
    initBase();   // both depend on the hero just chosen (initCharacters may have run them with the last one)
    initPopup();
    // A level's Shop also sells ammo for the guns he carries / her bows' arrows (ours).
    if (shopOn_ && hero_ != Hero::Kabuto && std::none_of(shopItems_.begin(), shopItems_.end(), [](const ShopItem& i) { return i.kind == 4; }))
        shopItems_.push_back({4, shopAt_, false});
    if (shopOn_ && hero_ == Hero::Mecc && std::none_of(shopItems_.begin(), shopItems_.end(), [](const ShopItem& i) { return i.kind == 5; }))
        shopItems_.push_back({5, shopAt_, false});  // mines (no mission hands them out; ours)
    if (hero_ == Hero::Kabuto) {
        // He comes out of the sea weakened at the start of his story (k1a) and of 2a ("Eat 5
        // Smarties to reach full strength and size"); full size elsewhere. Our reading.
        const std::string mn = toLower(assetWorld_->missionName());
        // k1a: grow to break the wooden gate; k1b: "another growth spurt" for the stone gate;
        // k2a: "grow once more and attain your full magnificent girth".
        kbGrow_ = mn.rfind("k1a", 0) == 0 ? 0.5f : (mn.rfind("k1b", 0) == 0 ? 0.65f : (mn.rfind("k2a", 0) == 0 ? 0.8f : 1.0f));
        if (const char* g = std::getenv("GVR_KABUTO_GROW")) kbGrow_ = std::max(0.3f, std::min(1.0f, float(std::atof(g))));  // test hook
        applyKabutoGrowth(kbGrow_);
        LOGI("Kabuto starts at %.0f%% of his full size", kbGrow_ * 100.0f);
    }
    // Kabuto's state starts afresh each mission (setHero keeps it from one Kabuto mission to the next),
    // and his Offspring, boulders and throws don't outlive him when another hero takes over.
    kb_ = KabutoState{};
    initKabutoPowers();
    initThrows();
    if (hero_ == Hero::Kabuto) kbAdren_ = kbGrow_ >= 0.999f ? 1.0f : 0.0f;  // at full size he comes in ready
    initSkirmish();  // a multiplayer map (w_M_*): teams and computer players
    if (title_) return;  // the main menu's scene: no checkpoint
    if (haveRestore_) {  // what he carried at the checkpoint
        if (pendingRestore_.launcher && launcherModel_) {
            hasLauncher_ = true;
            launcherAmmo_ = std::max(launcherAmmo_, std::min(launcherMax_, pendingRestore_.rockets < 0 ? launcherGive_ : pendingRestore_.rockets));
        }
        grenades_ = std::max(grenades_, std::min(grenadeMax_, pendingRestore_.grenades));
        mines_ = std::max(mines_, std::min(minesMax_, pendingRestore_.mines));
        initBows();
        for (int b = 0; b < kNumBows; ++b)
            if (pendingRestore_.bows[b] && bows_[b].model) {
                bows_[b].have = true;
                if (bows_[b].maxAmmo > 0) bows_[b].ammo = std::max(bows_[b].ammo, std::min(bows_[b].maxAmmo, pendingRestore_.arrows[b]));
            }
        for (int g = 0; g < kNumGuns; ++g)
            if (pendingRestore_.guns[g] && guns_[g].model) {
                guns_[g].have = true;
                guns_[g].ammo = std::max(guns_[g].ammo, std::min(guns_[g].maxAmmo, pendingRestore_.ammo[g]));
            }
        flares_ = std::max(0, std::min(kMaxFlares, pendingRestore_.flares));
        syringes_ = std::max(syringes_, std::min(kMaxSyringes, pendingRestore_.syringes));
        haveRestore_ = false;
    }
    writeCheckpoint();
    // The mission's ambient music (the world's race and chapter).
    ambientSet_ = ambientFor(worldName_);
    initJetski();
    if (const char* w = std::getenv("GVR_WEAPON")) weaponSel_ = std::atoi(w);  // test hook: start with this weapon in hand
    if (!weaponOwned(weaponSel_)) weaponSel_ = 0;
    LOGI("Mission %s: %zu objectives, jetpack %s", assetWorld_->missionName().c_str(), objectives_.size(),
         jet ? "available" : "not yet (story)");
}

void GameApp::updateSun(float dt) {
    EnvironmentParams& env = renderer_.environment();
    env.time = float(time_);
    // The world's own (daylight) colours, captured once per loaded world.
    if (envBaseWorld_ != worldName_ + "#" + std::to_string(reinterpret_cast<uintptr_t>(assetWorld_.get())) ||
        envBaseSea_ != env.seaLevel) {
        envBaseWorld_ = worldName_ + "#" + std::to_string(reinterpret_cast<uintptr_t>(assetWorld_.get()));
        envBaseSea_ = env.seaLevel;
        envBase_ = env;
    }
    sunAzimuth_ += dt * 2.0f * kPi / 1200.0f;
    if (!settings_.dayNight || title_) {
        // Always day: the sun circles once in 20 minutes between 30 and 62 degrees up.
        const float el = (46.0f + 16.0f * std::sin(sunAzimuth_ * 2.0f)) * kPi / 180.0f;
        env.sunDirection = Vec3{std::cos(el) * std::sin(sunAzimuth_), std::sin(el), std::cos(el) * std::cos(sunAzimuth_)} * -1.0f;
        env.sunColor = envBase_.sunColor; env.ambientColor = envBase_.ambientColor; env.fogColor = envBase_.fogColor;
        env.clearColor = envBase_.clearColor; env.skyHorizon = envBase_.skyHorizon; env.skyZenith = envBase_.skyZenith;
        env.waterColor = envBase_.waterColor;
        env.skyTint = {1, 1, 1}; env.night = 0; env.bakedLight = {1, 1, 1};
        return;
    }
    // Day and night (ours; the original's light is fixed per level): one cycle in 20 minutes,
    // about 12 of daylight, a warm dusk and dawn, ~5 minutes of moonlit night with stars.
    if (const char* p = std::getenv("GVR_DAY_PHASE")) { dayPhase_ = float(std::atof(p)); dt = 0; }  // test hook
    dayPhase_ += dt / 1200.0f;
    if (dayPhase_ >= 1.0f) dayPhase_ -= 1.0f;
    const float sinEl = std::sin(2.0f * kPi * dayPhase_) * 0.78f + 0.22f;  // sun height (sine of elevation)
    const float el = std::asin(std::max(-1.0f, std::min(1.0f, sinEl))) ;
    const float az = sunAzimuth_;
    const float d = std::max(0.0f, std::min(1.0f, (sinEl + 0.1f) / 0.3f));            // daylight 0..1
    const float day = d * d * (3.0f - 2.0f * d);
    const float dusk = std::max(0.0f, 1.0f - std::fabs(sinEl - 0.03f) / 0.28f);      // low sun: warm light
    auto mix3 = [](const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; };
    auto mul = [](const Vec3& a, const Vec3& b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; };
    const Vec3 warm{1.0f, 0.62f, 0.42f}, nightBlue{0.13f, 0.17f, 0.3f};
    Vec3 toLight;
    if (sinEl > -0.08f) {
        const float e = std::max(el, 0.03f);
        toLight = {std::cos(e) * std::sin(az), std::sin(e), std::cos(e) * std::cos(az)};
    } else {  // the moon, high on the other side
        const float e = 0.75f;
        toLight = {std::cos(e) * std::sin(az + kPi), std::sin(e), std::cos(e) * std::cos(az + kPi)};
    }
    env.sunDirection = toLight * -1.0f;
    env.sunColor = mix3(Vec3{0.22f, 0.27f, 0.42f}, mix3(envBase_.sunColor, mul(envBase_.sunColor, warm), dusk), day);
    env.ambientColor = mix3(mul(envBase_.ambientColor, Vec3{0.32f, 0.38f, 0.55f}), envBase_.ambientColor, day);
    const Vec3 skyT = mix3(Vec3{0.09f, 0.11f, 0.22f}, mix3(Vec3{1, 1, 1}, Vec3{1.15f, 0.72f, 0.55f}, dusk * 0.8f), day);
    env.skyTint = skyT;
    env.night = 1.0f - day;
    env.fogColor = mul(envBase_.fogColor, mix3(nightBlue, mix3(Vec3{1, 1, 1}, Vec3{1.2f, 0.78f, 0.6f}, dusk * 0.7f), day));
    env.clearColor = env.fogColor;
    env.skyHorizon = env.fogColor;
    env.skyZenith = mul(envBase_.skyZenith, mix3(Vec3{0.12f, 0.14f, 0.24f}, Vec3{1, 1, 1}, day));
    env.waterColor = envBase_.waterColor * (0.35f + 0.65f * day);
    env.bakedLight = mix3(Vec3{0.34f, 0.4f, 0.58f}, mix3(Vec3{1, 1, 1}, Vec3{1.08f, 0.86f, 0.72f}, dusk * 0.6f), day);
}

void GameApp::renderShadows() {
    renderer_.setTerrainDetail(quality_ < 2);  // the adaptive fallback drops the extra terrain layers too
    buildBatches();
    pushUnscaledRig(lastInput_.head, lastInput_.headValid);  // HUD at human scale (Kabuto)
    buildHud();
    popUnscaledRig();
    if (!weaponsTicked_) dropHeldWeaponState();  // no weapon update ran since the last frame
    weaponsTicked_ = false;
    renderShadowMap();
    renderReflection();
    renderScope();
    buildBowLines();
}

void GameApp::renderShadows(const EyeRenderInfo& left, const EyeRenderInfo& right) {
    const EyeRenderInfo* e[2] = {&left, &right};
    for (int k = 0; k < 2; ++k) {
        eyeFov_[k][0] = e[k]->fovLeft; eyeFov_[k][1] = e[k]->fovRight; eyeFov_[k][2] = e[k]->fovUp; eyeFov_[k][3] = e[k]->fovDown;
    }
    haveFov_ = true;
    renderShadows();
}

void GameApp::renderShadowMap() {
    if (quality_ >= 3) { renderer_.setShadowsActive(false); return; }
    if (!shadowsOn_ || !assetWorld_ || !assetWorld_->loaded()) { renderer_.setShadowsActive(false); return; }
    const Vec3 centre = playMode_ ? player_.position : rig_.worldFromTracking.position;
    // 48 m square around the Meccaryn (~2.3 cm texels at 2048); wider for a bigger hero (Kabuto:
    // ~140 m, so the trees a few of his steps away still cast shadows; texel size cost nothing on the Quest).
    const float half = 24.0f * std::sqrt(std::max(1.0f, heroScale_));
    renderer_.beginShadowPass(renderer_.sunShadowMatrix(centre, half));
    assetWorld_->renderShadowCasters(renderer_, centre, half * 1.5f);
    const bool cut = cutsceneActive();
    for (const auto& c : chars_) {
        if (!c.visible || length(c.pos - centre) > half * 1.4f) continue;
        c.model->renderShadow(renderer_, c.xf());
    }
    renderVillagers(true, centre, half * 1.4f);
    renderVimps(true, centre, half * 1.4f);
    renderJetski(true, centre);
    for (const auto& n : nests_)
        if (n.alive && n.model && length(n.pos - centre) < half * 1.4f)
            for (const auto& part : n.model->parts) renderer_.drawShadow(part.mesh, n.xf * Mat4::scale(Vec3{1, 1, 1} * n.shown));
    for (const auto& e : enemies_.enemies)
        if (e.slot >= 0 && length(e.pos - centre) < half * 1.4f)
            enemyPool_[size_t(e.slot)]->renderShadow(renderer_, Mat4::translation(e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0}) *
                                                                    Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, e.yaw)));
    renderReaperWeapons(true, centre, half * 1.4f);
    renderSkirmish(true, centre);
    // The player's own body (whole, also in first person) and Timmy on his back.
    if (playMode_ && playerModel_ && !cut && !playerGibbed_) playerModel_->renderShadowFull(renderer_, bodyXf_);
    if (playMode_ && timmyOn_ && !cut) timmy_->renderShadow(renderer_, timmyXf_);
    renderer_.endShadowPass();
    renderer_.setShadowsActive(true);
}

// Inverse of an affine matrix (rotation/scale/mirror + translation).
Mat4 affineInverse(const Mat4& m) {
    const float a = m.m[0], b = m.m[4], c = m.m[8], d = m.m[1], e = m.m[5], f = m.m[9], g = m.m[2], h = m.m[6], i = m.m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    Mat4 r = Mat4::identity();
    if (std::fabs(det) < 1e-12f) return r;
    const float k = 1.0f / det;
    r.m[0] = (e * i - f * h) * k;  r.m[4] = (c * h - b * i) * k;  r.m[8] = (b * f - c * e) * k;
    r.m[1] = (f * g - d * i) * k;  r.m[5] = (a * i - c * g) * k;  r.m[9] = (c * d - a * f) * k;
    r.m[2] = (d * h - e * g) * k;  r.m[6] = (b * g - a * h) * k;  r.m[10] = (a * e - b * d) * k;
    r.m[12] = -(r.m[0] * m.m[12] + r.m[4] * m.m[13] + r.m[8] * m.m[14]);
    r.m[13] = -(r.m[1] * m.m[12] + r.m[5] * m.m[13] + r.m[9] * m.m[14]);
    r.m[14] = -(r.m[2] * m.m[12] + r.m[6] * m.m[13] + r.m[10] * m.m[14]);
    return r;
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

void GameApp::writeCheckpoint() {
    if (savePath_.empty() || !assetWorld_ || assetWorld_->missionName().empty()) return;
    SaveGame g;
    g.world = worldName_;
    g.mission = assetWorld_->missionName();
    g.syringes = syringes_;
    g.launcher = hasLauncher_;
    g.rockets = launcherAmmo_;
    for (int k = 0; k < kNumGuns; ++k) { g.guns[k] = guns_[k].have; g.ammo[k] = guns_[k].ammo; }
    g.grenades = grenades_;
    g.mines = mines_;
    for (int k = 0; k < kNumBows; ++k) { g.bows[k] = bows_[k].have; g.arrows[k] = bows_[k].ammo; }
    g.flares = flares_;
    if (writeSaveGame(savePath_, g)) LOGI("Checkpoint saved: %s / %s", g.world.c_str(), g.mission.c_str());
    else LOGW("Checkpoint could not be written to %s", savePath_.c_str());
}

void GameApp::loadWorldQuality() {
    if (settingsPath_.empty()) return;
    const std::string path = settingsPath_.substr(0, settingsPath_.rfind('/') + 1) + "quality.txt";
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) worldQuality_[toLower(line.substr(0, eq))] = std::max(0, std::min(3, std::atoi(line.c_str() + eq + 1)));
    }
}

void GameApp::saveWorldQuality() const {
    if (settingsPath_.empty()) return;
    const std::string path = settingsPath_.substr(0, settingsPath_.rfind('/') + 1) + "quality.txt";
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
        const int measured = (w == "kstory3" || w == "rstory2") ? 2 : 0;
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
        ++quality_;
        perfGood_ = 0;
        if (sinceUp_ < 10.0f) upHold_ = 60.0f;  // the last step up didn't hold
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

void GameApp::renderReflection() {
    ++frameNo_;
    // Every other frame (every third at quality 1); the last one is reused in between.
    const uint32_t every = quality_ >= 1 ? 3u : 2u;
    if (quality_ >= 2) { renderer_.setReflectionActive(false); return; }
    if (frameNo_ % every != 0 && reflFresh_) return;
    reflFresh_ = false;
    renderer_.setReflectionActive(false);
    if (!reflOn_ || !haveFov_ || !assetWorld_ || !assetWorld_->loaded() || !lastInput_.headValid) return;
    if (load_.phase != LoadPhase::Idle && load_.phase != LoadPhase::FadeOut) return;
    const float sea = renderer_.environment().seaLevel;
    const Pose head = rig_.toWorld(lastInput_.head);
    if (head.position.y < sea + 0.05f) return;  // under water: no mirror
    // Mirror about the sea surface: M = T(sea) S(1,-1,1) T(-sea).
    Mat4 mirror = Mat4::identity();
    mirror.m[5] = -1.0f;
    mirror.m[13] = 2.0f * sea;
    ViewParams v;
    v.view = Mat4::fromPose(head.inverse()) * mirror;
    v.eyePosition = Vec3{head.position.x, 2.0f * sea - head.position.y, head.position.z};
    // Wide enough for both eyes, plus a margin for the wave distortion.
    const float m = 0.08f;
    const float l = std::min(eyeFov_[0][0], eyeFov_[1][0]) - m, rr = std::max(eyeFov_[0][1], eyeFov_[1][1]) + m;
    const float u = std::max(eyeFov_[0][2], eyeFov_[1][2]) + m, d = std::min(eyeFov_[0][3], eyeFov_[1][3]) - m;
    const float nearZ = 0.1f;
    // No water in view (looking up at the sky): skip the pass.
    bool seesDown = false;
    for (int c = 0; c < 4 && !seesDown; ++c) {
        const Vec3 ray = head.orientation.rotate(normalize(Vec3{std::tan((c & 1) ? rr : l), std::tan((c & 2) ? u : d), -1.0f}));
        seesDown = ray.y < -0.01f;
    }
    if (!seesDown) return;
    v.projection = Mat4::projectionFov(l, rr, u, d, nearZ, 0.0f);
    const uint32_t calls0 = renderer_.drawCallsThisFrame();
    renderer_.beginReflectionPass(v, nearZ);
    const bool cut = cutsceneActive();
    std::vector<int> cutTypes;
    if (cut) cutTypes = cutscene_->actorTypes();
    assetWorld_->setHiddenTypes(cutTypes);
    assetWorld_->render(renderer_, true);
    for (const auto& n : nests_)
        if (n.alive && n.shown >= 0.02f) drawModel(n.model, n.xf * Mat4::scale(Vec3{1, 1, 1} * n.shown), false);
    const Vec3 eye = head.position;
    if (playMode_ && playerModel_ && !cut && !playerGibbed_) playerModel_->renderFull(renderer_, bodyXf_);
    if (playMode_ && timmyOn_ && !cut) timmy_->render(renderer_, timmyXf_);
    if (playMode_ && gunValid_ && !cut) drawModel(heldModel(), gunXf_, false);
    for (const auto& c : chars_) {
        if (!c.visible || length(c.pos - eye) > (c.flyer ? 260.0f : 90.0f)) continue;
        if (cut && std::find(cutTypes.begin(), cutTypes.end(), c.type) != cutTypes.end()) continue;
        c.model->render(renderer_, c.xf());
    }
    for (const auto& e : enemies_.enemies)
        if (e.slot >= 0 && length(e.pos - eye) < 90.0f)
            enemyPool_[size_t(e.slot)]->render(renderer_, Mat4::translation(e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0}) *
                                                              Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, e.yaw)));
    if (cut) cutscene_->render();
    renderer_.endReflectionPass();
    renderer_.setReflectionActive(true);
    reflFresh_ = true;
    static int logged = 0;
    if (logged < 3) { ++logged; LOGI("Reflection pass: %u draw calls", renderer_.drawCallsThisFrame() - calls0); }
}

void GameApp::renderSunRays() {
    // Shafts of light around the sun: a slowly turning fan of faint additive rays far out on the
    // sky in the sun's direction. Terrain and objects in front hide them (depth test), so they read
    // as rays breaking past ridges and trees. (A cheap stand-in for screen-space god rays.)
    if (!sunRaysMesh_) {
        MeshData m;
        const int n = 14;
        for (int i = 0; i < n; ++i) {
            const float a = float(i) * 2.0f * kPi / float(n) + 0.17f * float(i % 3);
            const float len = 0.55f + 0.25f * float((i * 7) % 5) / 4.0f, w = 0.035f + 0.02f * float(i % 2);
            const uint32_t b = uint32_t(m.vertices.size());
            const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(a + kPi * 0.5f), sb = std::sin(a + kPi * 0.5f);
            const float pts[3][3] = {{cb * w * 0.3f, sb * w * 0.3f, 255}, {-cb * w * 0.3f, -sb * w * 0.3f, 255}, {ca * len, sa * len, 0}};
            for (const auto& p : pts) {
                Vertex v{};
                v.pos[0] = p[0];
                v.pos[1] = p[1];
                v.normal[2] = 1;
                v.color[0] = v.color[1] = v.color[2] = 255;
                v.color[3] = uint8_t(p[2]);
                m.vertices.push_back(v);
            }
            m.indices.insert(m.indices.end(), {b, b + 1, b + 2});
            (void)cb;
        }
        sunRaysMesh_ = renderer_.uploadMesh(m);
    }
    const EnvironmentParams& env = renderer_.environment();
    const Vec3 toSun = normalize(env.sunDirection * -1.0f);
    const Vec3 eye = renderer_.currentView().eyePosition;
    const float dist = 900.0f;
    // Billboard facing the viewer at the sun's position, rotating slowly.
    const Vec3 f = toSun * -1.0f;
    const Vec3 upHint = std::fabs(f.y) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 r = normalize(cross(upHint, f)), u = cross(f, r);
    Mat4 basis = Mat4::identity();
    basis.m[0] = r.x; basis.m[1] = r.y; basis.m[2] = r.z;
    basis.m[4] = u.x; basis.m[5] = u.y; basis.m[6] = u.z;
    basis.m[8] = -f.x; basis.m[9] = -f.y; basis.m[10] = -f.z;
    DrawParams dp;
    dp.blend = BlendMode::Additive;
    dp.unlit = true;
    dp.noFog = true;
    dp.doubleSided = true;
    dp.depthWrite = false;
    const float k = 0.10f + 0.03f * std::sin(float(time_) * 0.7f);
    dp.tint = {env.sunColor.x * k, env.sunColor.y * k, env.sunColor.z * k * 0.85f, 1.0f};
    renderer_.draw(sunRaysMesh_, Mat4::translation(eye + toSun * dist) * basis * Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, float(time_) * 0.02f)) *
                                     Mat4::scale({dist, dist, dist}), dp);
}

void GameApp::debugReloadWithBriefing(const InputFrame& in) {
    if (assetWorld_) beginLoading(assetWorld_->missionName(), in);
}

void GameApp::beginLoading(const std::string& mission, const InputFrame& in, bool load) {
    load_ = LoadState{};
    load_.phase = load ? LoadPhase::FadeOut : LoadPhase::Brief;
    load_.mission = load ? mission : std::string();
    // The briefing faces where the player looks now (head yaw), 2 m away.
    load_.yaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : camYaw_;
    missionText(mission, load_.title, load_.brief);
    LOGI("Loading %s (%s)", mission.c_str(), load_.title.c_str());
}

void GameApp::missionText(const std::string& mission, std::string& title, std::string& brief) const {
    title = mission;
    brief.clear();
    if (!gameText_) return;
    const std::string t = gameText_->get(mission);
    if (!t.empty()) title = t;
    // Objectives text: '^' = new line, '~N' = colour change (dropped).
    const std::string mo = gameText_->get("MO_" + mission);
    for (size_t i = 0; i < mo.size(); ++i) {
        if (mo[i] == '^') brief += '\n';
        else if (mo[i] == '~' && i + 1 < mo.size()) ++i;
        else brief += mo[i];
    }
    while (!brief.empty() && (brief.front() == '\n' || brief.front() == ' ')) brief.erase(0, 1);
    while (!brief.empty() && (brief.back() == '\n' || brief.back() == ' ')) brief.pop_back();
}

void GameApp::renderBriefOverlay() {
    if (!text_.ready() || !assetWorld_) return;
    std::string title, brief;
    missionText(assetWorld_->missionName(), title, brief);
    char prog[64];
    std::snprintf(prog, sizeof prog, "OBJECTIVES %zu / %zu", objectivesDone_, objectives_.size());
    float tw = 0, th = 0, bw = 0, bh = 0, pw = 0, ph = 0;
    const MeshId tm = text_.mesh(title, 0.095f, 1.5f, &tw, &th);
    const MeshId bm = brief.empty() ? 0 : text_.mesh(brief, 0.055f, 1.5f, &bw, &bh);
    const MeshId pm = text_.mesh(prog, 0.055f, 1.5f, &pw, &ph);
    // Head-locked, slightly below eye level, the world still visible around and through it.
    const Quat r = subRot_;
    const float total = th + 0.05f + bh + 0.05f + ph;
    const Vec3 c = subHead_ + r.rotate(Vec3{0, -0.08f, -1.6f});
    const Mat4 base = Mat4::translation(c) * Mat4::rotation(r);
    text_.drawPanel(base, std::max(std::max(tw, bw), pw) + 0.12f, total + 0.12f, {0, 0, 0, 0.5f});
    float y = 0.5f * total;
    auto put = [&](MeshId m, float h, const Vec4& col) {
        if (!m) return;
        y -= 0.5f * h;
        text_.draw(m, base * Mat4::translation({0, y, 0.004f}), col);
        y -= 0.5f * h + 0.05f;
    };
    put(tm, th, {1.0f, 0.85f, 0.4f, 1.0f});
    put(bm, bh, {0.92f, 0.92f, 0.88f, 1.0f});
    put(pm, ph, {0.6f, 0.95f, 0.6f, 1.0f});
}

void GameApp::updateLoading(float dt, const InputFrame& in) {
    load_.t += dt;
    if (in.headValid) headWorldRot_ = rig_.toWorld(in.head).orientation;
    switch (load_.phase) {
        case LoadPhase::FadeOut:
            if (load_.t > 0.5f) { load_.phase = LoadPhase::Show; load_.t = 0; }
            break;
        case LoadPhase::Show:
            // Two frames of the briefing on screen before the (blocking) load, so the headset keeps
            // showing it while the level is rebuilt.
            if (++load_.frames >= 3) {
                if (!load_.world.empty() && assetWorld_ && assetWorld_->switchWorld(load_.world)) {
                    worldName_ = load_.world;
                    streamsIndexed_ = false;
                    streamFiles_.clear();
                    load_.mission = assetWorld_->missions().empty() ? std::string() : assetWorld_->missions().front();
                    for (const auto& m : assetWorld_->missions())  // a checkpoint's mission
                        if (!load_.worldMission.empty() && toLower(m) == toLower(load_.worldMission)) load_.mission = m;
                    missionText(load_.mission, load_.title, load_.brief);
                }
                // A world without missions (the multiplayer maps) is still set up (its scenery, a skirmish).
                if (!load_.mission.empty() || (assetWorld_ && assetWorld_->missions().empty())) loadMission(load_.mission);
                load_.phase = LoadPhase::Brief;
                load_.t = 0;
            }
            break;
        case LoadPhase::Brief:
            if (load_.t > 6.0f) { load_.phase = LoadPhase::FadeIn; load_.t = 0; }
            break;
        case LoadPhase::FadeIn:
            load_.phase = LoadPhase::Idle;  // the spawn (and its opening cutscene, with its own fade) follows
            break;
        case LoadPhase::Idle:
            break;
    }
}

void GameApp::renderLoading() {
    if (!text_.ready()) return;
    const Vec3 eye = renderer_.currentView().eyePosition;
    const float black = load_.phase == LoadPhase::FadeOut ? std::min(1.0f, load_.t / 0.5f) : 1.0f;
    const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
    text_.drawPanel(Mat4::translation(eye + fwd * 0.1f) * Mat4::rotation(headWorldRot_), 1.0f, 1.0f, {0, 0, 0, black});
    if (load_.phase == LoadPhase::FadeOut) return;
    const Quat r = Quat::fromAxisAngle({0, 1, 0}, load_.yaw);
    const Vec3 base = eye + r.rotate(Vec3{0, 0, -2.0f});
    auto place = [&](float x, float up) { return Mat4::translation(base + r.rotate(Vec3{x, up, 0})) * Mat4::rotation(r); };
    // The original loading art of the story's race on the left (mecc_ / reaper_ / kabuto_loading),
    // the mission's title and objectives on the right.
    const Hero dest = !load_.world.empty() ? heroForWorld(load_.world) : hero_;
    const TextureId art = loadingArt(dest);
    const float fade = std::min(1.0f, (load_.phase == LoadPhase::Show ? 0.0f : 0.3f) + load_.t / 0.6f);
    float textX = 0.0f, wrap = 2.6f, titleWrap = 1.8f;
    if (art) {
        text_.drawImage(place(-0.66f, -0.08f), 1.12f, 0.84f, art, {1, 1, 1, fade});
        textX = 0.62f;
        wrap = titleWrap = 1.15f;
    }
    float w = 0, h = 0, th = 0;
    float y = art ? 0.32f : 0.42f;
    if (const MeshId t = text_.mesh(load_.title, art ? 0.1f : 0.14f, titleWrap, &w, &th)) {
        text_.draw(t, place(textX, y - 0.5f * th), {1.0f, 0.85f, 0.4f, 1.0f});
        y -= th + 0.06f;
    }
    if (!load_.brief.empty())
        if (const MeshId b = text_.mesh(load_.brief, art ? 0.05f : 0.055f, wrap, &w, &h)) text_.draw(b, place(textX, y - 0.5f * h), {0.9f, 0.9f, 0.85f, 1.0f});
    const float pulse = 0.55f + 0.45f * std::sin(load_.t * 4.0f);
    const char* label = load_.phase == LoadPhase::Show ? "LOADING..." : "GET READY";
    if (const MeshId l = text_.mesh(label, 0.07f, 1.5f, &w, &h)) text_.draw(l, place(0.0f, -0.62f), {1, 1, 1, pulse});
}

void GameApp::loadMission(const std::string& name) {
    AssetWorld* w = assetWorld_.get();
    // (An empty name: a world without missions, already placed by switchWorld.)
    if (!w || (name.empty() ? !w->missions().empty() : !w->selectMission(name))) { LOGW("Mission %s unavailable", name.c_str()); return; }
    chars_.clear();
    creatures_.creatures.clear();
    creatures_.spits.clear();
    enemies_.enemies.clear();
    rescue_ = RescueDirector{};
    nests_.clear();
    objectives_.clear();
    objectivesDone_ = 0;
    missionComplete_ = false;
    missionHostiles_ = false;
    advanceDone_ = false;
    piranhaKnown_ = false;
    blasts_.clear();
    gibs_.clear();
    playerHealth_ = 100.0f;
    initCharacters(w, w->models());
    initMission();
    needSpawn_ = true;  // to the mission's start marker; its opening cutscene follows
    {
        // Resident memory after each load (Linux/Android): a leak across mission loads would show
        // as steady growth here - the Quest closes apps that grow too large.
        long rssKb = -1;
        if (FILE* f = std::fopen("/proc/self/status", "r")) {
            char line[256];
            while (std::fgets(line, sizeof line, f))
                if (std::strncmp(line, "VmRSS:", 6) == 0) { rssKb = std::atol(line + 6); break; }
            std::fclose(f);
        }
        LOGI("Mission %s loaded (resident memory %ld MB)", name.c_str(), rssKb >= 0 ? rssKb / 1024 : -1L);
    }
}

void GameApp::completeObjective(int index) {
    if (index < 0 || size_t(index) >= objectives_.size() || objectives_[size_t(index)].done) return;
    objectives_[size_t(index)].done = true;
    ++objectivesDone_;
    haptics_.push_back({kLeft, 0.4f, 0.08f});
    LOGI("Objective %d done (%zu/%zu)", index, objectivesDone_, objectives_.size());
    if (objectivesDone_ < objectives_.size() || missionComplete_) return;
    missionComplete_ = true;
    LOGI("Mission %s complete", assetWorld_->missionName().c_str());
    haptics_.push_back({kLeft, 0.9f, 0.3f});
    haptics_.push_back({kRight, 0.9f, 0.3f});
    // The outro cutscene (op 0x19); where it can't be played, its soundtrack still carries the
    // dialogue. In 1a_1_timmy that is Timmy handing Baz the jetpack.
    const MissionData& m = assetWorld_->mission();
    if (!m.flick19.empty()) {
        startCutscene(m.flick19);
        if (!cutsceneActive()) playFlickSoundtrack(m.flick19);
    }
    if (!player_.jetpackAvailable) {
        player_.jetpackAvailable = true;
        LOGI("Jetpack received");
    }
    if (music_ && !streamLeft_) { music_->request("mvy"); stingerLeft_ = 8.0f; }
}

void GameApp::updateMission(float dt) {
    (void)dt;
    if (missionComplete_ || !playMode_) return;
    // Proximity: within 2 m (10 game units) of the object's bounds.
    const Vec3 p = player_.position + Vec3{0, 0.9f, 0};
    for (size_t i = 0; i < objectives_.size(); ++i) {
        const Objective& o = objectives_[i];
        if (o.done || o.kind != 1) continue;
        const Vec3 c{std::max(o.boundsMin.x, std::min(o.boundsMax.x, p.x)),
                     std::max(o.boundsMin.y, std::min(o.boundsMax.y, p.y)),
                     std::max(o.boundsMin.z, std::min(o.boundsMax.z, p.z))};
        if (length(c - p) <= 10.0f * gGameUnitsToMeters + (player_.radius - 0.35f)) completeObjective(int(i));
    }
    for (size_t i = 0; i < objectives_.size(); ++i) {
        const Objective& o = objectives_[i];
        if (!o.done && o.kind == 5 && o.item >= 0 && size_t(o.item) < destruct_.items.size() && !destruct_.items[size_t(o.item)].alive())
            completeObjective(int(i));
    }
}

namespace {
// Lists *.wav / *.raw files under `dir` (one level of subdirectories), lower-case name -> path.
void indexFiles(const std::string& dir, const char* ext, std::vector<std::pair<std::string, std::string>>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const std::string path = dir + "/" + n;
        std::string l = toLower(n);
        if (l.size() > 4 && l.compare(l.size() - 4, 4, ext) == 0) {
            out.push_back({l.substr(0, l.size() - 4), path});
        } else if (l.find('.') == std::string::npos) {
            indexFiles(path, ext, out);
        }
    }
    closedir(d);
}
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    const bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}
}  // namespace

void GameApp::initMusic() {
    if (!audio_ || !assetWorld_) return;
    std::vector<uint8_t> bytes;
    musicSet_ = std::make_unique<MusicSet>();
    std::string err;
    if (!assetWorld_->fs().read("MusicSet.bin", bytes) || !musicSet_->parse(bytes.data(), bytes.size(), &err)) {
        LOGW("Music: MusicSet.bin unavailable %s", err.c_str());
        musicSet_.reset();
        return;
    }
    // The game's Music folder sits next to Bin (copy it to the headset alongside Bin).
    for (const char* rel : {"/../Music", "/Music"}) indexFiles(dataDir_ + rel, ".raw", musicFiles_);
    LOGI("Music: %zu songs, %zu transitions, %zu segment files", musicSet_->songCount(), musicSet_->transitionCount(),
         musicFiles_.size());
    if (musicFiles_.empty()) { musicSet_.reset(); return; }
    music_ = std::make_unique<MusicDirector>(*musicSet_);
    // Ambient mood by story: Mecc a1 (the game also has a2, and Kabuto/Reaper sets).
    ambientSet_ = ambientFor(worldName_);
    music_->request(ambientSet_);
    audio_->setMusicVolume(0.8f * float(settings_.music) / 8.0f);
}

std::string GameApp::findMusicFile(const std::string& segment) const {
    for (const auto& f : musicFiles_)
        if (f.first == segment) return f.second;
    return {};
}

void GameApp::updateMusic(float dt) {
    if (!music_ || !audio_) return;
    streamLeft_ = std::max(0.0f, streamLeft_ - dt);
    stingerLeft_ = std::max(0.0f, stingerLeft_ - dt);
    // Mood from the fight around the player (our mapping; the original's is native code):
    // small battle (msb), large battle with 4+ attackers (mlb), victory stinger, ambient.
    size_t engaged = 0;
    for (const auto& c : creatures_.creatures)
        if (c.state != CreatureState::Dead && c.state != CreatureState::Idle && c.state != CreatureState::Return &&
            length(c.pos - player_.position) < 45.0f)
            ++engaged;
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead && e.state != EnemyState::Idle && length(e.pos - player_.position) < 60.0f) ++engaged;
    const char* race = ambientSet_.c_str();  // "ma1" -> prefix 'm'
    const std::string pre(1, race[0]);
    std::string want = ambientSet_;
    if (engaged > 0) combatHold_ = 6.0f;
    else combatHold_ = std::max(0.0f, combatHold_ - dt);
    if (combatHold_ > 0) want = pre + (engaged >= 4 ? "lb" : (pre == "k" ? "ff" : "sb"));  // Kabuto's sets have no sb
    if (combatHold_ > 0 && assetWorld_) {  // the world's own battle sets (op 109 / 111), if it names them
        const std::string& own = assetWorld_->world().music[engaged >= 4 ? 3 : 1];
        if (!own.empty()) want = toLower(own);
    }
    if (stingerLeft_ > 0) want = pre + "vy";
    if (streamLeft_ > 0) want.clear();  // a cutscene soundtrack is playing
    music_->request(want);
    static std::string lastMood = "?";
    if (want != lastMood) {
        LOGI("Music mood: %s", want.empty() ? "(soundtrack)" : want.c_str());
        lastMood = want;
    }
    // Keep one segment queued behind the playing one (mood changes land on segment ends).
    for (int guard = 0; guard < 2 && audio_->musicQueued() < 2; ++guard) {
        const std::string seg = music_->next();
        if (seg.empty()) break;
        const std::string path = findMusicFile(seg);
        std::vector<uint8_t> bytes;
        if (path.empty() || !readFile(path, bytes)) {
            static std::vector<std::string> warned;
            if (std::find(warned.begin(), warned.end(), seg) == warned.end()) {
                warned.push_back(seg);
                LOGW("Music segment %s missing", seg.c_str());
            }
            continue;
        }
        audio_->queueMusic(decodeMusicRaw(bytes.data(), bytes.size()), 22050);
    }
    if (want.empty() && audio_->musicQueued() > 0 && streamLeft_ > 0) audio_->clearMusic();
}

bool GameApp::playFlickSoundtrack(const std::string& flick) {
    if (!audio_) return false;
    std::vector<uint8_t> bytes;
    if (!assetWorld_->fs().read("flk_" + flick + ".bin", bytes)) return false;
    // The flick names its stream as a NUL-terminated "f_..." string.
    std::string stream;
    for (size_t i = 0; i + 3 < bytes.size() && stream.empty(); ++i) {
        if ((i == 0 || bytes[i - 1] == 0) && (bytes[i] == 'f' || bytes[i] == 'F') && bytes[i + 1] == '_') {
            size_t e = i;
            while (e < bytes.size() && bytes[e] >= 0x20 && bytes[e] < 0x7f) ++e;
            if (e < bytes.size() && bytes[e] == 0 && e - i > 2) stream = toLower(std::string(bytes.begin() + long(i), bytes.begin() + long(e)));
        }
    }
    if (stream.empty()) return false;
    return playStream(stream);
}

bool GameApp::playStream(const std::string& streamIn, bool cue) {
    if (!audio_) return false;
    const std::string stream = toLower(streamIn);
    if (!streamsIndexed_) {
        // Soundtracks and effects in Stream/<World>/, voice clips in StreamEnglish/<World>/.
        // Voice clips live in StreamEnglish/Newstreams (baz006, tim001, ...), effects and soundtracks
        // in Stream/<World>/ (indexFiles also walks one level of subfolders).
        for (const char* rel : {"/../Stream/", "/Stream/"}) indexFiles(dataDir_ + rel + worldName_, ".wav", streamFiles_);
        for (const char* rel : {"/../StreamEnglish", "/StreamEnglish"}) indexFiles(dataDir_ + rel, ".wav", streamFiles_);
        streamsIndexed_ = true;
    }
    for (const auto& f : streamFiles_) {
        if (f.first != stream) continue;
        const std::string key = "stream_" + stream;
        if (cue) {
            if (!audio_->hasSound(key)) {
                std::vector<uint8_t> wav;
                WavData w;
                std::string err;
                if (!readFile(f.second, wav) || !parseWav(wav.data(), wav.size(), w, &err)) break;
                audio_->addSound(key, std::move(w));
            }
            cueVoices_.push_back(audio_->play2D(key.c_str(), 1.0f));
            LOGI("Cutscene cue %s", stream.c_str());
            return true;
        }
        std::vector<uint8_t> wav;
        WavData w;
        std::string err;
        if (!readFile(f.second, wav) || !parseWav(wav.data(), wav.size(), w, &err)) break;
        const float seconds = float(w.mono.size()) / float(w.rate);
        audio_->addSound("stream_" + stream, std::move(w));
        streamVoice_ = audio_->play2D(("stream_" + stream).c_str(), 1.0f);
        streamLeft_ = seconds;
        audio_->clearMusic();
        LOGI("Cutscene soundtrack %s (%.0f s)", stream.c_str(), seconds);
        return true;
    }
    LOGW("Cutscene %s %s not found (copy the game's Stream and StreamEnglish folders next to Bin)", cue ? "sound" : "soundtrack",
         stream.c_str());
    return false;
}

bool GameApp::cutsceneActive() const { return cutscene_ && cutscene_->active(); }
size_t GameApp::cutsceneShots() const { return cutscene_ ? cutscene_->shotsPlayed() : 0; }

void GameApp::startCutscene(const std::string& first) {
    if (!cutscene_ || first.empty()) return;
    if (!cutscene_->start(first)) return;
    LOGI("Cutscene %s started", first.c_str());
    skipArmed_ = false;  // a trigger held when the cutscene starts must not skip it
}

void GameApp::followHead(float dt, const InputFrame& in) {
    // Head-locked panels (subtitles, the mission briefing): yaw and pitch of the head, no roll,
    // eased toward the head so small head motions don't shake the text.
    if (!in.headValid) return;
    const Pose head = rig_.toWorld(in.head);
    const Vec3 f = head.orientation.rotate(Vec3{0, 0, -1});
    const float yaw = std::atan2(-f.x, -f.z), pitch = std::asin(std::max(-1.0f, std::min(1.0f, f.y)));
    const Quat want = Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({1, 0, 0}, pitch);
    const float k = subInit_ ? std::min(1.0f, dt * 10.0f) : 1.0f;
    Quat q = subRot_;
    if (q.x * want.x + q.y * want.y + q.z * want.z + q.w * want.w < 0) q = Quat{-q.x, -q.y, -q.z, -q.w};
    subRot_ = normalize(Quat{q.x + (want.x - q.x) * k, q.y + (want.y - q.y) * k, q.z + (want.z - q.z) * k, q.w + (want.w - q.w) * k});
    subHead_ = subInit_ ? subHead_ + (head.position - subHead_) * std::min(1.0f, dt * 20.0f) : head.position;
    subInit_ = true;
}

void GameApp::updateCutscene(float dt, const InputFrame& in) {
    cutscene_->update(dt);
    const std::string stream = cutscene_->takeStream();
    if (!stream.empty()) playStream(stream);
    for (const auto& c : cutscene_->takeCues()) playStream(c, true);
    for (const auto& e : cutscene_->takeEffects()) {
        static const bool trace = std::getenv("GVR_CUT_TRACE") != nullptr;
        if (trace) LOGI("cut-fx %s at (%.1f %.1f %.1f)", e.name.c_str(), e.pos.x, e.pos.y, e.pos.z);
        cutsceneEffect(e.name, e.pos);
    }
    fx_.update(dt);  // the main loop's updateFx is paused while a cutscene plays
    updateDataFx(dt);
    if (in.headValid) headWorldRot_ = rig_.toWorld(in.head).orientation;
    // Skip with either trigger.
    bool skip = false;
    if (in.hand[0].trigger < 0.3f && in.hand[1].trigger < 0.3f) skipArmed_ = true;
    else if (skipArmed_) skip = in.hand[0].trigger > 0.6f || in.hand[1].trigger > 0.6f;
    if (skip) {
        cutscene_->stop();
        if (audio_ && streamVoice_) audio_->stop(streamVoice_);
        if (audio_)
            for (uint32_t v : cueVoices_) audio_->stop(v);
        cueVoices_.clear();
        streamVoice_ = 0;
        streamLeft_ = 0;
        LOGI("Cutscene skipped");
    }
    Vec3 eye;
    float yaw;
    // Keep the viewpoint above the ground: a few shot cameras sit at or below our terrain
    // surface (e.g. story1_jetpack_2's low angle), which would put the viewer inside a hill.
    auto aboveGround = [&](Vec3 p) {
        float h;
        if (assetWorld_ && assetWorld_->terrain().heightAt(p.x, p.z, h)) p.y = std::max(p.y, h + 0.7f);
        return p;
    };
    if (cutscene_->active()) cutBlack_ = cutscene_->blackout();
    if (cutscene_->active() && cutscene_->takeNewShot(eye, yaw)) {
        eye = aboveGround(eye);
        // Put the viewer's eyes at the shot camera, facing the scene; head look stays free.
        cutEye_ = eye;
        cutYaw_ = yaw;
        const float headYaw = in.headValid ? yawOf(in.head.orientation) : 0.0f;
        const Quat q = Quat::fromAxisAngle({0, 1, 0}, yaw - headYaw);
        const Vec3 headT = in.headValid ? in.head.position * rig_.comfort.worldScale : Vec3{0, 1.6f, 0};
        rig_.worldFromTracking.orientation = q;
        rig_.worldFromTracking.position = eye - q.rotate(headT);
        subInit_ = false;  // the subtitles jump with the cut
        if (std::getenv("GVR_CUT_TRACE")) {
            const Vec3 got = rig_.toWorld(in.head).position;
            LOGI("cut eye: shot (%.1f %.1f %.1f) head (%.2f %.2f %.2f) scale %.2f -> (%.1f %.1f %.1f)", eye.x, eye.y, eye.z, in.head.position.x,
                 in.head.position.y, in.head.position.z, rig_.comfort.worldScale, got.x, got.y, got.z);
        }
    } else if (cutscene_->active()) {
        // Travel with a moving shot camera (translation only; the view never rotates by itself).
        const Vec3 now = aboveGround(cutscene_->eyeNow());
        rig_.worldFromTracking.position = rig_.worldFromTracking.position + (now - cutEye_);
        cutEye_ = now;
    }
    followHead(dt, in);
    if (!cutscene_->active()) {
        // Back to the game where the player stands.
        if (playMode_) rig_.teleportHead(in, player_.position, player_.yaw);
        // A cutscene that faded out (its last shot's script) fades back in on the game.
        respawnFade_ = std::max(respawnFade_, cutBlack_);
        cutBlack_ = 0;
        LOGI("Cutscene finished");
    }
}

void GameApp::renderCutscene() {
    cutscene_->render();
    const Vec3 eye = renderer_.currentView().eyePosition;
    // Subtitles: a panel below the line of sight, fixed for the shot.
    if (gameText_ && text_.ready() && settings_.subtitles) {
        std::string lines;
        for (const auto& key : cutscene_->subtitles()) {
            const std::string t = gameText_->get(key);
            if (t.empty()) continue;
            if (!lines.empty()) lines += "\n";
            lines += t;
        }
        if (!lines.empty()) {
            float w = 0, h = 0;
            const MeshId m = text_.mesh(lines, 0.052f, 1.3f, &w, &h);
            // Head-locked (user request after device test 19): the panel follows the headset,
            // a little below the line of sight, smoothed so it doesn't jitter with the head.
            const Quat r = subRot_;
            const Vec3 at = subHead_ + r.rotate(Vec3{0, -0.34f, -1.4f});
            const Mat4 xf = Mat4::translation(at) * Mat4::rotation(r * Quat::fromAxisAngle({1, 0, 0}, 0.24f));
            text_.drawPanel(xf, w + 0.08f, h + 0.05f, {0, 0, 0, 0.55f});
            text_.draw(m, xf * Mat4::translation({0, 0, 0.003f}), {1.0f, 0.92f, 0.7f, 1.0f});
        }
    }
    const float b = cutscene_->blackout();
    if (b > 0.01f && text_.ready()) {
        const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
        text_.drawPanel(Mat4::translation(eye + fwd * 0.1f) * Mat4::rotation(headWorldRot_), 1.0f, 1.0f, {0, 0, 0, b});
    }
}

void GameApp::spawnWave() {
    if (enemyPool_.empty()) return;
    ++wave_;
    // Three Reapers ~35 m out in front of the player (spread left/right), on solid terrain.
    const float base = player_.yaw;
    int placed = 0;
    for (int attempt = 0; attempt < 24 && placed < 3; ++attempt) {
        const float a = base + (frand() - 0.5f) * 1.4f;
        const float r = 30.0f + frand() * 10.0f;
        const Vec3 p = player_.position + Vec3{-std::sin(a), 0, -std::cos(a)} * r;
        float h;
        if (!assetWorld_->terrain().heightAt(p.x, p.z, h)) continue;
        if (h < 0.5f) continue;  // not in the sea
        enemies_.spawn(p, a + kPi);  // facing the player
        ++placed;
    }
    LOGI("Wave %d: %d Reapers", wave_, placed);
}

void GameApp::damagePlayer(float amount, DeathCause cause) {
    if (playerHealth_ <= 0 || dying_) return;
    static const bool god = std::getenv("GVR_GOD") != nullptr;  // test hook: the player can't be hurt (perf runs)
    if (god) return;
    if (hero_ == Hero::Kabuto) amount *= 0.3f;  // a giant's hide: Reaper bolts and bites sting
    playerHealth_ -= amount;
    lastHurt_ = cause;
    if (playerHealth_ < 45.0f && playerHealth_ > 0 && cause != DeathCause::Fall) timmySay(frand() < 0.5f ? "smchbaseattack4" : "smchbaseattack1");
    sinceDamage_ = 0;
    damageFlash_ = 1.0f;
    if (hurtCooldown_ <= 0) {
        static const char* hurt[] = {"me_shrt1", "me_shrt2", "me_shrt3"};
        sfx2D(hero_ == Hero::Kabuto ? "ka_wound" : hurt[int(frand() * 2.99f)], 0.8f);
        hurtCooldown_ = 0.6f;
    }
    // A jolt in the hands, stronger for a bigger hit and on the side it came from.
    {
        const float a = std::min(1.0f, 0.35f + amount / 40.0f);
        float side = 0.0f;  // -1 left .. +1 right
        if (hitDirTimer_ > 1.1f && lastInput_.headValid) {
            const Pose head = rig_.toWorld(lastInput_.head);
            Vec3 d = hitFrom_ - head.position;
            d.y = 0;
            if (length(d) > 1e-3f) side = dot(normalize(d), head.orientation.rotate(Vec3{1, 0, 0}));
        }
        haptics_.push_back({kLeft, a * (1.0f - 0.45f * std::max(0.0f, side)), 0.12f});
        haptics_.push_back({kRight, a * (1.0f + 0.45f * std::min(0.0f, side)), 0.12f});
    }
    // The player bleeds too (bites and bullets).
    if (cause != DeathCause::Drowned) bleed(player_.position + Vec3{0, 1.2f, 0}, Vec3{frand() - 0.5f, 0.3f, frand() - 0.5f}, false, 1.0f);
    if (playerHealth_ <= 0) startDeath();
}

void GameApp::startDeath() {
    playerHealth_ = 0;
    dying_ = true;
    deathTime_ = 0;
    deathCause_ = player_.swimming() ? DeathCause::Drowned : lastHurt_;
    deathWasFp_ = firstPerson_;
    firstPerson_ = false;  // see the body: the camera steps back from it
    tpAiming_ = false;
    armTargets_[0].valid = armTargets_[1].valid = false;
    haptics_.push_back({kLeft, 1.0f, 0.4f});
    haptics_.push_back({kRight, 1.0f, 0.4f});
    if (deathCause_ == DeathCause::Blast && hero_ != Hero::Mecc) deathCause_ = DeathCause::Fall;  // only the Meccaryn has chunks
    if (deathCause_ == DeathCause::Blast) {
        // Blown apart: the original Meccaryn chunks (MC_chunk1..6), blood everywhere.
        playerGibbed_ = true;
        const Vec3 at = player_.position + Vec3{0, 1.0f, 0};
        for (int k = 0; k < 6; ++k) {
            if (!meccChunk_[k]) continue;
            Gib g;
            g.model = meccChunk_[k];
            g.pos = at + Vec3{(frand() - 0.5f) * 0.6f, (frand() - 0.3f) * 0.8f, (frand() - 0.5f) * 0.6f};
            g.vel = Vec3{(frand() - 0.5f) * 9.0f, 4.0f + 5.0f * frand(), (frand() - 0.5f) * 9.0f};
            g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
            g.angle = frand() * 6.28f;
            g.spin = 5.0f + 6.0f * frand();
            g.scale = 1.0f;
            g.life = -4.0f;  // lie a little longer than the others
            gibs_.push_back(g);
        }
        blood_.burst(at, Vec3{0, 1, 0}, 40, 7.0f, 0.09f);
        blood_.pool(player_.position, 2.0f);
        sfx("me_mslex", at, 1.0f, 0.9f);
        sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", at, 1.0f);
    } else {
        playerGibbed_ = false;
        sfx2D(hero_ == Hero::Kabuto ? (frand() < 0.5f ? "ka_deth1" : "ka_deth2") : "me_shrt3", 1.0f, 0.85f);
        if (deathCause_ != DeathCause::Drowned) blood_.pool(player_.position, 1.4f);
    }
    if (skirmish_ && skNet_ != SkNetMode::Join) sk_.localDied(skLocal_);  // (a joiner's death goes to the host with its state)
    static const char* const kCause[] = {"shot", "bitten", "eaten in the water", "blown apart", "fell"};
    LOGI("Player died (%s)", kCause[int(deathCause_)]);
}

void GameApp::updateDeath(float dt, const InputFrame& in) {
    deathTime_ += dt;
    PlayerInput none;
    none.viewYaw = player_.yaw;
    player_.update(dt, none);
    placeCamera(dt, false, &in);
    float sink = 0;
    if (deathCause_ == DeathCause::Drowned) {
        sink = std::max(0.0f, deathTime_ - 1.2f) * 0.5f;  // goes under
        if (frand() < dt * 3.0f) bloodInWater(player_.position, 0.8f);
        if (deathTime_ < 4.0f && frand() < dt * 3.5f) piranhaBite(player_.position, 1.2f, true);  // the frenzy goes on
    }
    bodyXf_ = Mat4::translation(player_.position + Vec3{0, heroLift_ - sink, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, player_.yaw));
    if (hero_ == Hero::Kabuto && kbGrowShown_ < 0.999f) bodyXf_ = bodyXf_ * Mat4::scale(Vec3{1, 1, 1} * kbGrowShown_);
    if (playerModel_ && !playerGibbed_) {
        playerModel_->setHideEnabled(false);
        const int clip = deathClip_[deathCause_ == DeathCause::Drowned ? 1 : (deathCause_ == DeathCause::Fall ? 2 : 0)];
        if (clip >= 0) {
            const float d = playerModel_->animDuration(clip);
            const float t = std::min(deathTime_, d * 0.999f);
            playerModel_->poseBlend(renderer_, clip, t, clip, t, 0.0f);
        }
    }
    timmyOn_ = false;
    if (deathTime_ > 5.5f) {
        // Back at the mission's start, whole again.
        dying_ = false;
        playerGibbed_ = false;
        playerHealth_ = 100.0f;
        firstPerson_ = deathWasFp_;
        piranhaAttack_ = false;
        waterTime_ = 0;
        if (skirmish_) teleportPlayer(sk_.spawnPoint(skLocal_), player_.yaw);  // at his team's base
        else teleportPlayer(assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
        respawnFade_ = 1.0f;
        LOGI("Player respawned");
    }
}

void GameApp::explode(const Vec3& at, float radius, float damage, bool fire, int hurtsPlayer) {
    if (at.y < player_.seaLevel + 1.0f && (!enemies_.groundHeight || enemies_.groundHeight(at.x, at.z) < player_.seaLevel - 0.2f))
        waterSplash(at, std::min(3.0f, 0.5f * radius), 2);  // in (or just over) the water
    creatures_.disturb(at, std::max(15.0f, radius * 4.0f));  // the ground shakes: buried Rippers come up
    if (hero_ == Hero::Kabuto) rockHit(at, radius);          // a blow at a volcanic rock pillar's top
    // With the original effects on (Options), the blast is fx.bin's (its own sound too).
    const std::string fxName = blastFx_;
    blastFx_.clear();
    const bool dataFx = !fxName.empty() && playDataFx(fxName, at);
    if (fire && !dataFx) {
        blasts_.push_back({at, 3, 0.0f});
        sfx("me_mslex", at, 1.0f, 0.95f + 0.1f * frand());
    }
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Enemy& e = enemies_.enemies[i];
        if (e.state == EnemyState::Dead) continue;
        const float d = length(e.pos + Vec3{0, 0.9f, 0} - at);
        if (d > radius) continue;
        const bool killed = enemies_.damage(int(i), damage * (1.0f - 0.6f * d / radius), /*blast=*/true);
        if (killed && enemies_.enemies[i].objective >= 0) completeObjective(enemies_.enemies[i].objective);
        bleed(enemies_.enemies[i].pos + Vec3{0, 1.0f, 0}, normalize(enemies_.enemies[i].pos - at + Vec3{0, 0.5f, 0}), killed, 1.0f);
        if (killed) blood_.pool(enemies_.enemies[i].pos, 1.3f);
    }
    skirmishBlast(at, radius, damage);
    enemies_.alertNear(at, 30.0f);
    // Bodies near the blast are torn apart.
    for (auto& e : enemies_.enemies)
        if (e.state == EnemyState::Dead && e.stateTime > 0.6f && !e.gibbed && length(e.pos - at) < radius * 0.8f) {
            e.gibbed = true;
            gibCorpse(e.pos, e.yaw, e.pos - at, true, 1.0f);
        }
    for (auto& c : creatures_.creatures)
        if (c.state == CreatureState::Dead && c.stateTime > 0.6f && !c.exploded && !c.gibbed && length(c.pos - at) < radius * 0.8f) {
            c.gibbed = true;
            gibCorpse(c.pos, c.yaw, c.pos - at, false, c.scale);
        }
    for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
        const Creature& c = creatures_.creatures[i];
        if (c.state == CreatureState::Dead) continue;
        const float d = length(c.pos + Vec3{0, 0.5f * c.scale, 0} - at);
        if (d > radius) continue;
        std::vector<CreatureEvent> ev;
        Vec3 dir = c.pos - at;
        dir.y = 0;
        dir = length(dir) > 1e-3f ? normalize(dir) : Vec3{0, 0, -1};
        const int slot = c.slot;
        const float scale = c.scale;
        if (creatures_.damage(int(i), damage * (1.0f - 0.6f * d / radius), dir, &ev) && slot >= 0 && size_t(slot) < chars_.size() &&
            chars_[size_t(slot)].objective >= 0)
            completeObjective(chars_[size_t(slot)].objective);
        for (const auto& e : ev) {
            if (e.kind == CreatureEvent::Die && e.burst) spawnGore(e.pos, e.dir, scale);
            creatureSound(e);
        }
    }
    for (auto& rv : race_.racers)  // Reaperski rivals in the blast spin out
        if (!rv.finished && length(rv.pos - at) < radius + 3.0f) rv.spin = 3.0f;
    for (size_t i = 0; i < chars_.size(); ++i) {
        const CharacterInst& ci = chars_[i];
        if (!ci.boss || ci.hp <= 0) continue;
        const float db = std::max(0.0f, length(ci.pos + Vec3{0, 0.5f * ci.bossH, 0} - at) - ci.bossR);
        if (db < radius) hitBoss(int(i), damage * (1.0f - 0.6f * db / radius), ci.pos + Vec3{0, 0.5f * ci.bossH, 0}, normalize(ci.pos - at + Vec3{0, 0.5f, 0}));
    }
    for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
        const Vimp& v = vimps_.vimps[i];
        const float dv = length(v.pos + Vec3{0, 2.0f, 0} - at);
        if (v.state != VimpState::Dead && dv < radius + 1.5f)
            hitVimp(int(i), damage * (1.0f - 0.6f * std::min(1.0f, dv / (radius + 1.5f))), v.pos + Vec3{0, 2.0f, 0}, normalize(v.pos - at + Vec3{0, 0.5f, 0}));
    }
    // Scenery in the blast: distance to the object's upright axis, less its size.
    for (size_t k = 0; k < destruct_.items.size(); ++k) {
        const Destructible& it = destruct_.items[k];
        if (!it.alive()) continue;
        const float y = std::max(it.base.y, std::min(it.base.y + it.height, at.y));
        const float dd = std::max(0.0f, length(Vec3{it.base.x, y, it.base.z} - at) - 0.5f * it.radius);
        if (dd < radius) damageObject(int(k), damage * (1.0f - 0.5f * dd / radius), true, at);
    }
    for (size_t k = 0; k < nests_.size(); ++k) {
        const float dn = length(nests_[k].pos - at);
        if (dn < radius + nests_[k].radius * 0.4f) damageNest(int(k), damage * (1.0f - 0.5f * std::min(1.0f, dn / radius)));
    }
    LOGI("%s at (%.1f %.1f %.1f), %zu creatures alive", fire ? "Rocket exploded" : "Quake", at.x, at.y, at.z, creatures_.aliveCount());
    // His own rockets throw the Meccaryn (rocket jumps; a blast under him kills his fall speed)
    // and hurt him the closer he is: up to 40 at point blank, nothing at the blast's edge.
    const Vec3 rel = player_.position + Vec3{0, 0.9f, 0} - at;
    const bool hurts = hurtsPlayer < 0 ? fire : hurtsPlayer != 0;
    const float dp = hurts ? length(rel) : 1e9f;  // a quake (Kabuto's stomp) doesn't throw its maker
    if (playMode_ && dp < radius && !dying_) {
        const float f = 1.0f - dp / radius;
        const float hurt = 40.0f * f * std::sqrt(f);
        if (hurt > 1.0f) {
            hitFrom_ = at;
            hitDirTimer_ = 1.2f;
            damagePlayer(hurt, DeathCause::Blast);
        }
    }
    if (playMode_ && dp < radius * 1.2f) {
        const float k = 1.0f - dp / (radius * 1.2f);
        const Vec3 dir = dp > 1e-3f ? rel * (1.0f / dp) : Vec3{0, 1, 0};
        player_.velocity.x += dir.x * 9.0f * k;
        player_.velocity.z += dir.z * 9.0f * k;
        player_.velocity.y = std::max(player_.velocity.y, 0.0f) + (4.0f + std::max(0.0f, dir.y) * 8.0f) * k;
        player_.onGround = false;
        haptics_.push_back({kLeft, 0.8f * k, 0.15f});
        haptics_.push_back({kRight, 0.8f * k, 0.15f});
    }
    // Reapers standing near the blast are shoved.
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Vec3 r = enemies_.enemies[i].pos - at;
        const float d = length(r);
        if (d < radius * 1.5f && d > 1e-3f) enemies_.push(int(i), r * (6.0f * (1.0f - d / (radius * 1.5f)) / d));
    }
}

void GameApp::updateRockets(float dt) {
    for (auto& r : rockets_) {
        r.life += dt;
        if (r.kind == 2) {
            // A SAM's homing missile: turns toward the player (limited rate), bursts close to him,
            // on the ground or when its fuel runs out.
            // A burning flare in reach draws it off the player.
            Vec3 chest = player_.position + Vec3{0, 0.5f * player_.height, 0};
            Vec3 flare;
            if (flareTarget(r.pos, flare)) chest = flare;
            const Vec3 to = chest - r.pos;
            const float dist = length(to);
            const float sp = length(r.vel);
            Vec3 f = r.vel * (1.0f / std::max(1e-3f, sp));
            const Vec3 want = to * (1.0f / std::max(1e-3f, dist));
            const float k = std::min(1.0f, (1.8f + 40.0f / std::max(5.0f, dist)) * dt);  // tighter when close: no orbiting
            f = normalize(f + (want - f) * k);
            r.vel = f * std::min(30.0f, sp + 10.0f * dt);
            if (!rocketTrail(r) && frand() < dt * 25.0f) smokePuff(FxTex::Smoke, r.pos, Vec3{0, 0.3f, 0}, 0.3f, 1.4f, 1.2f, 0.45f, 0.1f);
            float g = -1e9f;
            const bool ground = assetWorld_ && assetWorld_->terrain().heightAt(r.pos.x, r.pos.z, g) && r.pos.y < g + 0.3f;
            if (dist < 3.0f || ground || r.pos.y < 0.0f || r.life > 9.0f || dying_) {
                LOGI("SAM missile bursts %.1f m from the player", length(r.pos - (player_.position + Vec3{0, 0.5f * player_.height, 0})));
                explode(r.pos, 3.5f, 60.0f);
                r.life = 1e9f;
                continue;
            }
            r.pos += r.vel * dt;
            continue;
        }
        if (r.kind == 5 || r.kind == 6) {
            // Lobbed: a Millenium bomb bounces once and bursts at its next landing; a grenade bounces
            // and rolls until its fuse (2.5 s) runs out. Either bursts on whoever it strikes.
            const bool gren = r.kind == 6;
            r.vel.y -= 9.8f * dt;
            const float st = length(r.vel) * dt;
            const Vec3 d5 = normalize(r.vel);
            float t5 = st;
            Vec3 n5{0, 1, 0};
            int tg = -1;
            bool h5 = st > 1e-5f && weapon_.raycast && weapon_.raycast(r.pos, d5, st, t5, n5, tg);
            if (!h5 && r.pos.y + r.vel.y * dt < 0.0f) { h5 = true; t5 = st; n5 = {0, 1, 0}; tg = -1; }  // the sea
            const bool fuse = gren ? r.life > 2.5f : r.life > 5.0f;
            if (!fuse && h5 && tg == -1 && r.pos.y > 0.0f && (gren || r.bounces == 0)) {
                r.pos += d5 * std::max(0.0f, t5 - 0.05f);
                r.vel = (r.vel - n5 * (2.0f * dot(r.vel, n5))) * (gren ? 0.4f : 0.45f);  // off the slope, losing most of its speed
                ++r.bounces;
                if (r.bounces <= 3 && length(r.vel) > 1.0f) sfx(gren ? "bi_dirt1" : "me_mtrht", r.pos, gren ? 0.5f : 0.8f, 1.0f + 0.1f * frand());
                continue;
            }
            if (h5 || fuse) {
                const Vec3 at = h5 ? r.pos + d5 * t5 : r.pos;
                if (gren) {
                    // weapon 2: 80 damage, splash 75 over (.., 30, 50) - near the rocket's 80 / (.., 30, 55).
                    blastFx_ = "FX_EXPLOSION_GRENADE";
                    if (playDataFx(blastFx_, at)) {
                        blastFx_.clear();
                        explode(at + Vec3{0, 0.3f, 0}, 3.6f, 130.0f, false, 1);
                    } else {
                        blastFx_.clear();
                        explode(at + Vec3{0, 0.3f, 0}, 3.6f, 130.0f, false, 1);
                        blasts_.push_back({at + Vec3{0, 0.3f, 0}, 2, 0.0f});
                        sfx("me_grnex", at, 1.0f, 0.95f + 0.1f * frand());
                    }
                    float near = 1e9f;
                    for (const auto& e : enemies_.enemies)
                        if (e.state != EnemyState::Dead) near = std::min(near, length(e.pos + Vec3{0, 0.9f, 0} - at));
                    LOGI("Grenade bursts %.1f m away (nearest Reaper %.1f m from it, %d bounces)", length(at - player_.position), near, r.bounces);
                } else {
                    // weapon 1: 150 damage, splash 150 over (.., 50, 80) - against the rocket's 80 / (.., 30, 55).
                    blastFx_ = "FX_EXPLOSION_LBOMB";
                    explode(at, 5.5f, 225.0f);
                }
                r.life = 1e9f;
                continue;
            }
            r.pos += r.vel * dt;
            continue;
        }
        if (r.kind == 8) {
            // Delphi's homing arrow: turns (limited rate) toward the nearest living foe within 60 degrees of
            // its flight and 80 m; a faint green trail.
            const float sp = length(r.vel);
            Vec3 f = r.vel * (1.0f / std::max(1e-3f, sp));
            Vec3 best;
            float bestD = 80.0f;
            int bestId = -1;
            auto consider = [&](const Vec3& p, int id) {
                const Vec3 to = p - r.pos;
                const float d = length(to);
                if (d < bestD && dot(to * (1.0f / std::max(1e-3f, d)), f) > 0.5f) { bestD = d; best = p; bestId = id; }
            };
            for (size_t i = 0; i < enemies_.enemies.size(); ++i)
                if (enemies_.enemies[i].state != EnemyState::Dead) consider(enemies_.enemies[i].pos + Vec3{0, 1.1f, 0}, int(i));
            for (size_t i = 0; i < creatures_.creatures.size(); ++i)
                if (creatures_.creatures[i].state != CreatureState::Dead)
                    consider(creatures_.creatures[i].pos + Vec3{0, 0.5f * creatures_.creatures[i].scale, 0}, kCreatureTarget + int(i));
            for (size_t i = 0; i < vimps_.vimps.size(); ++i)
                if (vimps_.vimps[i].state != VimpState::Dead) consider(vimps_.vimps[i].pos + Vec3{0, 2.0f, 0}, kVimpTarget + int(i));
            if (bestD < 1.0f && bestId >= 0 && weapon_.onHit) {
                // Close enough: it strikes the foe it was chasing (and bursts).
                hitMul_ = r.dmg;
                weapon_.onHit(bestId, best, f);
                hitMul_ = 1.0f;
                explode(best, 1.8f, 40.0f, false, 1);
                sfx("me_mslex", best, 0.9f, 1.1f);
                r.life = 1e9f;
                continue;
            }
            if (bestD < 80.0f) {
                if (r.bounces == 0) { r.bounces = 1; LOGI("Homing arrow locked on a foe %.0f m away", bestD); }
                const Vec3 want = normalize(best - r.pos);
                f = normalize(f + (want - f) * std::min(1.0f, 8.0f * dt));
                r.vel = f * sp;
            }
            if (!rocketTrail(r) && frand() < dt * 20.0f) smokePuff(FxTex::FireGreen, r.pos, Vec3{0, 0.1f, 0}, 0.12f, 0.3f, 0.4f, 0.6f, 0.0f);
        }
        if (r.kind == 3) {
            // A Reaper's bazooka rocket: smoke trail; bursts near the player as well as on what it hits.
            if (!rocketTrail(r) && frand() < dt * 25.0f) smokePuff(FxTex::SmokeGrey, r.pos, Vec3{0, 0.3f, 0}, 0.25f, 1.2f, 1.0f, 0.45f, 0.1f);
            if (length(r.pos - (player_.position + Vec3{0, 0.5f * player_.height, 0})) < 1.6f) {
                explode(r.pos, 3.5f, 55.0f);
                r.life = 1e9f;
                continue;
            }
        }
        const float step = length(r.vel) * dt;
        const Vec3 dir = normalize(r.vel);
        float t;
        Vec3 n;
        int target = -1;
        bool hit = weapon_.raycast && weapon_.raycast(r.pos, dir, step, t, n, target);
        if (!hit) {
            // The sea stops rockets too.
            const Vec3 next = r.pos + r.vel * dt;
            if (next.y < 0.0f && r.pos.y >= 0.0f) { hit = true; t = step * (r.pos.y / std::max(1e-4f, r.pos.y - next.y)); }
        }
        if (hit || r.life > 5.0f) {
            if (r.kind == 1) explode(hit ? r.pos + dir * t : r.pos, 2.6f, 75.0f);  // fire bolt: smaller blast
            else if (r.kind == 3) explode(hit ? r.pos + dir * t : r.pos, 3.5f, 55.0f);
            else if (r.kind == 4 || r.kind == 7 || r.kind == 8) {
                // Shotgun shell (weapon 6: 40 damage, splash 40) or an RPG-bow arrow (fxb_bow_rpg: the
                // same numbers): the hit itself like a bullet of that weight, then a small burst round it.
                const Vec3 at = hit ? r.pos + dir * t : r.pos;
                if (hit && target != -1 && weapon_.onHit) {
                    hitMul_ = r.dmg > 0 ? r.dmg : guns_[kGunShotgun].damage;
                    weapon_.onHit(target, at, dir);
                    hitMul_ = 1.0f;
                }
                explode(at, 1.8f, 40.0f, false, 1);
                if (!playDataFx("fx_Explosion_shotgun", at)) {  // weapon 6's / the RPG bow's impact effect
                    blasts_.push_back({at, 1, 0.0f});
                    sfx(r.kind == 4 ? "me_rsgex" : "me_mslex", at, 0.9f, 0.95f + 0.1f * frand());
                }
            }
            else {
                blastFx_ = "FX_EXPLOSION_ROCKET";
                explode(hit ? r.pos + dir * t : r.pos, 4.0f, 120.0f);
            }
            r.life = 1e9f;
            continue;
        }
        r.pos += r.vel * dt;
    }
    syncRocketFx();
    rockets_.erase(std::remove_if(rockets_.begin(), rockets_.end(), [](const Rocket& r) { return r.life >= 1e9f; }), rockets_.end());
}

void GameApp::updateNests(float dt) {
    if (nests_.empty()) return;
    // Which pooled Ripper models are in use (a creature refers to them)?
    std::vector<char> used(chars_.size(), 0);
    for (const auto& c : creatures_.creatures)
        if (c.slot >= 0 && size_t(c.slot) < used.size()) used[size_t(c.slot)] = 1;
    for (size_t i = 0; i < nests_.size(); ++i) {
        NestInst& n = nests_[i];
        n.shown += (n.target - n.shown) * std::min(1.0f, dt * 6.0f);  // shrinks smoothly when hit
        if (!n.alive) continue;
        if (n.dying >= 0) {
            n.dying += dt;
            n.target = 0.0f;
            if (n.dying > 0.6f) { n.alive = false; LOGI("Ripper nest %zu destroyed", i); }
            continue;
        }
        if (!playMode_ || playerHealth_ <= 0 || length(n.pos - player_.position) > 70.0f) continue;
        n.timer -= dt;
        if (n.timer > 0) continue;
        n.timer = 6.0f + 4.0f * frand();
        // Up to three of its brood at a time.
        int brood = 0;
        for (const auto& c : creatures_.creatures) brood += c.hunter && c.state != CreatureState::Dead && length(c.home - n.pos) < 1.0f;
        if (brood >= 3) continue;
        for (size_t k = 0; k < chars_.size(); ++k) {
            CharacterInst& ci = chars_[k];
            if (!ci.pooled || used[k] || (ci.size == CreatureSize::Small) != n.small) continue;
            const Vec3 to = player_.position - n.pos;
            const float yaw = std::atan2(-to.x, -to.z);
            const ObjectType* t = assetWorld_->registry().type(uint32_t(ci.type));
            const float hp = t && t->hitPoints > 0 ? t->hitPoints : 40.0f;
            const int idx = creatures_.spawnHunter(n.pos, yaw, ci.scale, ci.size, hp);
            creatures_.creatures[size_t(idx)].slot = int(k);
            used[k] = 1;
            ci.objective = -1;
            sfx(ci.size == CreatureSize::Small ? "rs_spawn" : "rm_spawn", n.pos, 0.9f);
            LOGI("Ripper nest %zu: a Ripper hatches", i);
            break;
        }
    }
}

void GameApp::damageNest(int i, float amount) {
    if (i < 0 || size_t(i) >= nests_.size()) return;
    NestInst& n = nests_[size_t(i)];
    if (!n.alive || n.dying >= 0) return;
    n.hp -= amount;
    // It shrinks as it is hurt, and goes when it is spent.
    n.target = 0.35f + 0.65f * std::max(0.0f, n.hp / n.maxHp);
    sfx("ex_bdys1", n.pos + Vec3{0, 0.6f, 0}, 0.8f, 0.8f + 0.2f * frand());
    if (n.hp <= 0) {
        n.dying = 0;
        blasts_.push_back({n.pos + Vec3{0, 0.6f, 0}, 3, 0.0f});
        sfx("ex_bdyl1", n.pos, 1.0f);
        sfx("rl_deat1", n.pos, 0.8f, 0.7f);
    }
}

void GameApp::startSchool(School& s) {
    s.active = true;
    s.biteTimer = 0.4f;
    for (int i = 0; i < kPiranhas; ++i) s.fish[i] = Piranha{float(i) * 1.2566f + frand(), -1.0f};
}

void GameApp::updateSchools(float dt) {
    // School 0 hunts the player; the others the Smarties in the water (walkthrough: they devour a
    // Smartie in about five seconds).
    if (schools_.empty()) schools_.resize(1);
    School& ps = schools_[0];
    if (piranhaAttack_ && !ps.active) startSchool(ps);
    ps.active = piranhaAttack_;
    if (ps.active) ps.centre = player_.position;
    for (size_t i = 0; i < rescue_.smarties.size(); ++i) {
        const RescueSmartie& sm = rescue_.smarties[i];
        School* found = nullptr;
        for (size_t k = 1; k < schools_.size(); ++k)
            if (schools_[k].target == int(i)) found = &schools_[k];
        const bool prey = sm.state == SmartieState::Drowning;
        if (prey && !found) {
            School sc;
            sc.target = int(i);
            sc.centre = sm.pos;
            startSchool(sc);
            schools_.push_back(sc);
            continue;
        }
        if (found) {
            found->active = prey;
            if (prey) {
                found->centre = Vec3{sm.pos.x, 0.0f, sm.pos.z};
                found->biteTimer -= dt;
                if (found->biteTimer <= 0) {
                    found->biteTimer = 0.55f + 0.3f * frand();
                    sfx(frand() < 0.5f ? "pi_swrm1" : "pi_swrm2", sm.pos, 0.9f, 0.95f + 0.2f * frand());
                    piranhaBite(sm.pos, 0.75f, false);
                }
            }
        }
    }
    for (auto& sc : schools_) {
        sc.vis += ((sc.active ? 1.0f : 0.0f) - sc.vis) * std::min(1.0f, dt * 2.5f);
        if (sc.vis <= 0.01f) continue;
        for (int i = 0; i < kPiranhas; ++i) {
            Piranha& f = sc.fish[i];
            f.angle += dt * (1.6f + 0.25f * float(i));
            if (f.jump >= 0) f.jump += dt;
            if (f.jump > 2.3f) f.jump = -1;
            if (f.jump < 0 && sc.active && frand() < dt * (sc.target < 0 ? 0.25f : 0.6f)) f.jump = 0;  // leaps at the prey
        }
    }
    schools_.erase(std::remove_if(schools_.begin() + 1, schools_.end(), [](const School& sc) { return !sc.active && sc.vis < 0.01f; }),
                   schools_.end());
}

void GameApp::updateCombat(float dt) {
    GVR_PROF("combat (all)");
    { GVR_PROF("rockets"); updateRockets(dt); }
    { GVR_PROF("nests"); updateNests(dt); }
    // Water: entering it splashes; swimming leaves rings. Piranhas live in all deep water (the 1b
    // briefing warns of them): after a moment there they gather round and bite; leaving deep water
    // (shallows, the shore, the jetpack) sends them away. Rates and damage ours.
    const bool swim = playMode_ && player_.swimming();
    if (swim && !wasSwimming_) waterSplash(player_.position, 1.2f, 2);
    { GVR_PROF("waterflow"); updateWaterFlow(dt); }
    wasSwimming_ = swim;
    for (auto& f : waterFx_) f.age += dt;
    waterFx_.erase(std::remove_if(waterFx_.begin(), waterFx_.end(), [](const WaterFx& f) { return f.age > 2.0f; }), waterFx_.end());
    if (swim) {
        waterTime_ += dt;
        if (waterTime_ > 0.8f) {
            if (!piranhaAttack_) {
                piranhaAttack_ = true;
                LOGI("Piranhas!");
            }
            biteTimer_ -= dt;
            if (biteTimer_ <= 0) {
                biteTimer_ = 0.5f;
                sfx(frand() < 0.5f ? "pi_swrm1" : "pi_swrm2", player_.position, 1.0f, 0.9f + 0.2f * frand());
                damagePlayer(8.0f, DeathCause::Drowned);
                piranhaBite(player_.position, 1.0f, true);
                for (int h = 0; h < 2; ++h) haptics_.push_back({h, 0.6f, 0.08f});
            }
        }
    } else {
        waterTime_ = 0;
        biteTimer_ = 0;
        piranhaAttack_ = false;
    }
    // Wading and swimming: the water round the legs (updateWaterFlow).
    // NPCs in the water leave rings too (Rippers and Reapers floating or wading).
    {
        size_t k = 0;
        const size_t n = creatures_.creatures.size() + enemies_.enemies.size();
        if (npcRipple_.size() != n) npcRipple_.assign(n, 0.0f);
        auto ripple = [&](const Vec3& p, bool moving, float size) {
            float& t = npcRipple_[k++];
            t -= dt;
            if (p.y < -0.15f && t <= 0 && length(p - player_.position) < 60.0f && waterFx_.size() < 48) {
                t = moving ? 0.45f : 1.2f;
                waterFx_.push_back({Vec3{p.x, 0.03f, p.z}, 0.0f, 1, size});
            }
        };
        for (const auto& c : creatures_.creatures) ripple(c.pos, c.speed > 0.3f && c.state != CreatureState::Dead, 1.2f * c.scale);
        for (const auto& e : enemies_.enemies) ripple(e.pos, e.state == EnemyState::Advance, 1.6f);
    }
    { GVR_PROF("schools"); updateSchools(dt); }
    // Health regenerates after 4 s without damage.
    sinceDamage_ += dt;
    if (sinceDamage_ > 4.0f && !dying_) playerHealth_ = std::min(100.0f, playerHealth_ + 12.0f * dt);
    respawnFade_ = std::max(0.0f, respawnFade_ - dt * 1.2f);
    damageFlash_ = std::max(0.0f, damageFlash_ - dt * 3.0f);

    { GVR_PROF("enemies"); enemies_.update(dt, player_.position, playMode_ && playerHealth_ > 0 && !dying_, shooters_); }
    bushEvents();
    { GVR_PROF("skirmish"); updateSkirmish(dt); }
    // Model slots follow the enemies (the director removes sunk corpses).
    std::fill(slotUsed_.begin(), slotUsed_.end(), 0);
    for (auto& e : enemies_.enemies)
        if (e.slot >= 0 && size_t(e.slot) < slotUsed_.size()) slotUsed_[size_t(e.slot)] = 1;
    for (auto& e : enemies_.enemies)
        if (e.slot < 0)
            for (size_t k = 0; k < slotUsed_.size(); ++k)
                if (!slotUsed_[k]) { e.slot = int(k); slotUsed_[k] = 1; slotLift_[k] = enemyFootOffset_; break; }
    // Skinning is the costly part: Reapers too far to make out (the giant sees further) keep their
    // last pose; mid-distance ones are re-posed every other frame.
    const float poseFar = hero_ == Hero::Kabuto ? 400.0f : 130.0f;
    ++enemyPoseTick_;
    for (auto& e : enemies_.enemies) {
        if (e.slot < 0) continue;
        const float pd = length(e.pos - player_.position);
        const float near = hero_ == Hero::Kabuto ? 75.0f : 25.0f;
        const uint32_t every = pd < near ? 1u : (pd < near * 2.4f ? 2u : 4u);  // animation LOD by distance
        if (pd > poseFar || ((enemyPoseTick_ + uint32_t(e.slot)) % every) != 0) continue;
        SkinnedModel& m = *enemyPool_[size_t(e.slot)];
        m.poseBlend(renderer_, int(e.prevClip), e.prevTime, int(e.clip), e.clipTime, e.fadeWeight(enemies_.params.crossfade));
        // Ground contact from the actual pose: the lowest point (planted foot, or the body lying
        // in a death pose) rests on the ground. Smoothed lightly against single-frame pops.
        Vec3 mn, mx;
        m.posedBounds(mn, mx);
        float& lift = slotLift_[size_t(e.slot)];
        lift += (-mn.y - lift) * std::min(1.0f, dt / 0.06f);
    }
    placeReaperWeapons();  // the guns in their hands, from this frame's poses
    // Enemy shots from each Reaper's gun nozzle toward the player's chest, with spread.
    for (auto& e : enemies_.enemies) e.rocketCool = std::max(0.0f, e.rocketCool - dt);
    for (int i : shooters_) {
        Enemy& e = enemies_.enemies[size_t(i)];
        const Quat r = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
        Vec3 muzzle = e.pos + Vec3{0, 1.5f, 0} + r.rotate(Vec3{0.3f, 0, -0.6f});
        Mat4 nz;
        if (reaperMuzzle(e, muzzle)) {
        } else if (e.slot >= 0 && enemyPool_[size_t(e.slot)]->nodeWorld("ref_nozzle", nz))
            muzzle = e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0} + r.rotate(giantsToRuntime(nz.m[12], nz.m[13], nz.m[14]));
        if (e.weapon == 1 && e.weaponOn) {
            // Bazooka: one slow rocket every few seconds instead of the burst (ours).
            if (e.rocketCool > 0) continue;
            e.rocketCool = 3.5f;
            const Vec3 aim = player_.position + Vec3{0, 0.5f * player_.height, 0};
            Vec3 dir = normalize(aim - muzzle);
            dir = normalize(dir + Vec3{frand() - 0.5f, (frand() - 0.5f) * 0.3f, frand() - 0.5f} * (0.04f + 0.002f * length(aim - muzzle)));
            rockets_.push_back({muzzle + dir * 0.8f, dir * 22.0f, 0.0f, 3});
            sfx("pr_wsmil", muzzle, 0.9f, 1.1f);
            continue;
        }
        const Vec3 target = player_.position + Vec3{0, (0.5f + 0.4f * frand()) * player_.height, 0};
        Vec3 dir = normalize(target - muzzle);
        // Spread grows with distance: long shots mostly miss, close ones are dangerous.
        const float spread = 0.06f + 0.004f * length(target - muzzle);
        dir = normalize(dir + Vec3{frand() - 0.5f, (frand() - 0.5f) * 0.6f, frand() - 0.5f} * spread);
        enemyFire_.spawnShot(muzzle, dir);
        sfx("rh_sprsh", muzzle, 0.8f, 0.9f + 0.2f * frand());
    }
    // Raiders firing at a base building (game_app_base.cpp sets Enemy::siege).
    for (int i : enemies_.siegeShots) {
        const Enemy& e = enemies_.enemies[size_t(i)];
        const Quat r = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
        Vec3 muzzle = e.pos + Vec3{0, 1.5f, 0} + r.rotate(Vec3{0.3f, 0, -0.6f});
        reaperMuzzle(e, muzzle);
        Vec3 dir = normalize(e.siegeAt - muzzle);
        dir = normalize(dir + Vec3{frand() - 0.5f, (frand() - 0.5f) * 0.4f, frand() - 0.5f} * 0.08f);
        enemyFire_.spawnShot(muzzle, dir);
        sfx("rh_sprsh", muzzle, 0.6f, 0.9f + 0.2f * frand());
    }
    enemyFire_.update(dt, false, Vec3{}, Vec3{0, 0, -1});
    // Next wave after the last Reaper has fallen.
    { GVR_PROF("chars"); updateCharacters(dt); }
    { GVR_PROF("gore"); updateGore(dt); }
    { GVR_PROF("destruction"); updateDestruction(dt); }
    { GVR_PROF("burn"); updateBurn(); }
    { GVR_PROF("villagers"); updateVillagers(dt); }
    { GVR_PROF("vimps"); updateVimps(dt); }
    { GVR_PROF("bosses"); updateBosses(dt); }
    { GVR_PROF("sams"); updateSams(dt); }
    { GVR_PROF("fx"); updateFx(dt); }
    { GVR_PROF("spells"); stepSpells(dt); }
    // Sentries killed by their fall.
    for (int i : enemies_.landed) {
        const Enemy& e = enemies_.enemies[size_t(i)];
        if (e.objective >= 0) completeObjective(e.objective);
        blood_.pool(e.pos, 1.4f);
        bleed(e.pos + Vec3{0, 0.4f, 0}, Vec3{0, 1, 0}, true, 1.0f);
        sfx("me_hdfl1", e.pos, 1.0f, 0.8f);
    }
    { GVR_PROF("mission"); updateMission(dt); }
    if (enemies_.aliveCount() == 0 && !missionHostiles_ && objectives_.empty() && !skirmish_) {
        waveTimer_ += dt;
        if (waveTimer_ > 8.0f) { waveTimer_ = 0; spawnWave(); }
    } else {
        waveTimer_ = 0;
    }
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
    updateGunKick(dt);
    // Right stick forward: next weapon (released to neutral before it switches again).
    const float sy = in.hand[gh].thumbstick.y;
    if (sy < 0.3f) switchArmed_ = true;
    if (switchArmed_ && sy > 0.75f) {
        // The weapon strip drops in either way (showing what is carried); with only the gun
        // there is nothing to switch to.
        switchArmed_ = false;
        // Handgun -> shotgun -> machine gun -> sniper gun -> mortar -> grenades -> launcher -> bush pack ->
        // pop-up bomb, skipping what he doesn't carry.
        static const int kOrder[10] = {0, 4, 5, 6, 7, 8, 9, 1, 2, 3};
        int at = 0;
        while (at < 10 && kOrder[at] != weaponSel_) ++at;
        for (int k = 1; k <= 10; ++k) {
            const int next = kOrder[(at + k) % 10];
            if (weaponOwned(next)) { weaponSel_ = next; break; }
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
        static const char* kSelNames[10] = {"gun", "rocket launcher", "bush pack", "pop-up bomb", "", "", "", "", "grenades", "mines"};
        LOGI("Weapon: %s", gs >= 0 ? guns_[gs].name : kSelNames[std::max(0, std::min(9, weaponSel_))]);
    }
    weaponsTicked_ = true;
    scopeWanted_ = false;
    bowValid_ = bowLinesShown_ = bowDrawing_ = false;  // (a Reaper mission's bow)
    if (weaponSel_ == 2 || weaponSel_ == 3 || weaponSel_ == 8 || weaponSel_ == 9) {
        // In the bush / holding a bomb or grenade: no gun in hand (its bolts still fly).
        gunValid_ = false;
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
    }
    const bool trigger = in.hand[gh].trigger > 0.6f;
    rocketCooldown_ -= dt;
    if (launcher) {
        // One rocket per pull, 1 s to reload. The gun's bolts still fly (update with no trigger).
        weapon_.update(dt, false, muzzle, fwd);
        if (trigger && !rocketTriggerHeld_ && launcherAmmo_ <= 0) {
            sfx2D("me_outam", 0.8f);  // no rockets left
            haptics_.push_back({gh, 0.2f, 0.03f});
        }
        if (trigger && gunValid_ && rocketCooldown_ <= 0 && !rocketTriggerHeld_ && launcherAmmo_ > 0) {
            --launcherAmmo_;
            rockets_.push_back({muzzle, fwd * 32.0f, 0.0f});
            rocketCooldown_ = 1.0f;
            haptics_.push_back({gh, 0.9f, 0.12f});
            gunKick(0.30f);
            sfx("me_msls1", muzzle, 1.0f, 0.95f + 0.1f * frand());
        }
        rocketTriggerHeld_ = trigger;
        return;
    }
    rocketTriggerHeld_ = trigger;
    if (fireGun(dt, trigger, gunValid_, muzzle, fwd)) return;  // shotgun, machine gun, sniper gun
    if (weapon_.update(dt, trigger && gunValid_, muzzle, fwd)) {
        haptics_.push_back({gh, 0.35f, 0.04f});
        gunKick(0.10f);
        sfx((weapon_.shotsFired() & 1) ? "me_peas1" : "me_peas2", muzzle, 0.7f, 0.95f + 0.1f * frand());
    }
}

void GameApp::drawModel(const RenderModel* m, const Mat4& xf, bool additive, float alpha) {
    if (!m) return;
    for (const auto& part : m->parts) {
        DrawParams dp;
        dp.texture = part.texture;
        if (additive) {
            dp.blend = BlendMode::Additive;
            dp.unlit = true;
            dp.depthWrite = false;
            dp.doubleSided = true;
            dp.tint = {alpha, alpha, alpha, 1.0f};
        } else {
            dp.blend = part.blend;
            if (part.glass) { dp.blend = BlendMode::AlphaBlend; dp.tint.w *= 0.3f; }
            dp.unlit = !part.lit;
        }
        renderer_.draw(part.mesh, xf, dp);
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
    }
}

bool GameApp::hasOriginalAssets() const { return assetWorld_ && assetWorld_->loaded(); }

void GameApp::update(float dt, const InputFrame& in) {
    warmedThisFrame_ = false;
    GVR_PROF("update (all)");
    time_ += dt;
    if (dt > 1e-4f) fpsSmooth_ += (1.0f / dt - fpsSmooth_) * std::min(1.0f, dt * 2.0f);
    updateQuality(dt);
    { GVR_PROF("sun"); updateSun(dt); }
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
        const Vec3 sp = skirmish_ ? sk_.spawnPoint(skLocal_) : assetWorld_->spawnPoint();
        const float sy = skirmish_ ? player_.yaw : assetWorld_->spawnYaw();
        rig_.teleportHead(in, sp, sy);
        if (playMode_) {
            teleportPlayer(sp, sy);
            if (!missionHostiles_ && objectives_.empty() && !skirmish_) spawnWave();  // test waves only where the mission has no enemies or goals
            // The mission's opening cutscene (op 0x15), e.g. Baz's lander drop in 1a.
            const MissionData& m = assetWorld_->mission();
            // Test hook (headless): GVR_CUTSCENE=<first shot> plays that cutscene instead; "none" skips it.
            const char* forced = std::getenv("GVR_CUTSCENE");
            if (forced && std::string(forced) == "none") {}
            else if (forced && *forced) startCutscene(forced);
            else if (!m.startFlick.empty()) startCutscene(m.startFlick);
        }
        needSpawn_ = false;
    }
    if (cutsceneActive()) {
        updateCutscene(dt, in);
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

void GameApp::renderEye(const EyeRenderInfo& eye) { renderViews(&eye, 1); }

void GameApp::renderStereo(const EyeRenderInfo& left, const EyeRenderInfo& right) {
    const EyeRenderInfo eyes[2] = {left, right};
    renderViews(eyes, 2);
}

void GameApp::beginEyeViews(const EyeRenderInfo* eyes, int n, const Pose* worldEyeOverride, bool clear) {
    ViewParams vs[2];
    for (int k = 0; k < n; ++k) {
        const Pose ew = worldEyeOverride ? *worldEyeOverride : rig_.toWorld(eyes[k].trackingPose);
        vs[k].view = Mat4::fromPose(ew.inverse());
        vs[k].projection = Mat4::projectionFov(eyes[k].fovLeft, eyes[k].fovRight, eyes[k].fovUp, eyes[k].fovDown, 0.05f, 0.0f);
        vs[k].eyePosition = ew.position;
    }
    if (n == 2) renderer_.beginStereoView(vs, eyes[0].width, eyes[0].height, clear);
    else renderer_.beginView(vs[0], eyes[0].width, eyes[0].height, clear);
}

// n = 1: one eye into the bound framebuffer; n = 2: both eyes at once into a two-layer (multiview)
// framebuffer. The game's own distance checks then use the point between the eyes.
void GameApp::renderViews(const EyeRenderInfo* eyes, int n) {
    for (int i = 0; i < n; ++i) {
        // Remember the eyes' fields of view (the reflection camera covers both).
        const EyeRenderInfo& eye = eyes[i];
        const int k = eye.fovLeft < -eye.fovRight ? 0 : 1;
        eyeFov_[k][0] = eye.fovLeft; eyeFov_[k][1] = eye.fovRight; eyeFov_[k][2] = eye.fovUp; eyeFov_[k][3] = eye.fovDown;
        if (k == 1) haveFov_ = true;
        if (!haveFov_) { eyeFov_[1][0] = eye.fovLeft; eyeFov_[1][1] = eye.fovRight; eyeFov_[1][2] = eye.fovUp; eyeFov_[1][3] = eye.fovDown; }
    }
    if (load_.phase == LoadPhase::Brief && load_.t > 0.3f && load_.t < 5.0f && assetWorld_ && assetWorld_->loaded() && !warmedThisFrame_) {
        // Warm-up behind the briefing (first eye only): draw the new level from where the player
        // starts, a different direction each frame, so the driver's first-use work on its textures
        // and shaders happens now - on the Quest the first 3-4 s of play ran at 33-45 fps without it.
        const float yaw = float(warmFrame_ % 8) * 0.785398f;
        Pose wp;
        wp.position = assetWorld_->spawnPoint() + Vec3{0, 1.7f * heroScale_, 0};
        wp.orientation = Quat::fromAxisAngle({0, 1, 0}, yaw) * Quat::fromAxisAngle({1, 0, 0}, (warmFrame_ / 8) % 2 ? -0.35f : 0.0f);
        beginEyeViews(eyes, n, &wp, true);
        assetWorld_->render(renderer_);
        for (const auto& c : chars_)
            if (c.visible && c.model && length(c.pos - wp.position) < 160.0f) c.model->render(renderer_, c.xf());
        renderer_.endView();
        warmedThisFrame_ = true;
        if (++warmFrame_ == 16) LOGI("Warm-up: the level drawn from the start in 16 directions");
    }
    if (load_.phase != LoadPhase::Brief) warmFrame_ = 0;
    beginEyeViews(eyes, n, nullptr, true);
    const ViewParams v = renderer_.currentView();  // (stereo: the point between the eyes)
    if (load_.phase != LoadPhase::Idle && load_.phase != LoadPhase::FadeOut) {
        renderLoading();  // the briefing on black, nothing else
        renderer_.endView();
        return;
    }
    const bool cut = cutsceneActive();
    std::vector<int> cutTypes;
    if (cut) cutTypes = cutscene_->actorTypes();
    if (assetWorld_) assetWorld_->setHiddenTypes(cutTypes);
    if (assetWorld_ && assetWorld_->loaded()) assetWorld_->render(renderer_);
    for (const auto& n : nests_) {
        if (!n.alive || n.shown < 0.02f) continue;
        drawModel(n.model, n.xf * Mat4::scale(Vec3{1, 1, 1} * n.shown), false);
    }
    if (assetWorld_ && assetWorld_->loaded()) renderSunRays();
    if (playMode_ && playerModel_ && !cut && !playerGibbed_) playerModel_->render(renderer_, bodyXf_);
    if (playMode_ && !cut && !dying_) renderJetFx(firstPerson_);
    if (playMode_ && !cut && !dying_) renderBush();
    if (playMode_ && swordValid_ && swordModel_ && !cut && !dying_) drawModel(swordModel_, swordXf_, false);
    if (!cut) renderBow();
    // Timmy on the back: seen in third person (and in reflections and shadows). In first person he
    // sits right behind your head and the body turns with it, so he'd only ever flicker at the edge
    // of view: not drawn there.
    if (playMode_ && timmyOn_ && !cut && !firstPerson_) timmy_->render(renderer_, timmyXf_);
    if (playMode_ && gunValid_ && !cut && !dying_) {
        drawModel(heldModel(), gunDrawXf(), false);
        drawScopeLens();
    }
    if (playMode_ && !cut && !dying_ && !playerGibbed_) {  // the other weapon, holstered on the body
        if (holsterGun_) drawModel(gunModel_, holsterGunXf_, false);
        if (holsterLauncher_ && !firstPerson_) drawModel(launcherModel_, holsterLauncherXf_, false);
    }
    if (rocketModel_)
        for (const auto& r : rockets_) {
            if (r.kind == 1) continue;  // Delphi's bolt is all fire (particles)
            if (r.kind == 6) {
                if (grenadeModel_) {
                    const float k = 0.45f;
                    drawModel(grenadeModel_, Mat4::translation(r.pos) * Mat4::rotation(Quat::fromAxisAngle(normalize(Vec3{0.3f, 1, 0.2f}), r.life * 7.0f)) *
                                                 Mat4::scale({k, k, k}) * Mat4::translation((grenadeModel_->boundsMin + grenadeModel_->boundsMax) * -0.5f), false);
                }
                continue;
            }
            if (r.kind == 5 && lbombModel_) {  // the Millenium bomb, tumbling
                const float k = 0.35f;
                drawModel(lbombModel_, Mat4::translation(r.pos) * Mat4::rotation(Quat::fromAxisAngle(normalize(Vec3{1, 0.3f, 0.2f}), r.life * 9.0f)) *
                                           Mat4::scale({k, k, k}) * Mat4::translation((lbombModel_->boundsMin + lbombModel_->boundsMax) * -0.5f), false);
                continue;
            }
            if (r.kind == 7 || r.kind == 8) {  // an RPG-bow / homing arrow: the streak (rparrow) along its flight
                if (arrowModel_) {
                    const Vec3 f = normalize(r.vel);
                    const Quat yawQ = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
                    const Quat look = yawQ * Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, f.y))));
                    drawModel(arrowModel_, Mat4::translation(r.pos) * Mat4::rotation(look) * Mat4::scale({0.25f, 0.25f, 0.25f}), true, 0.9f);
                }
                continue;
            }
            if (r.kind == 4) {  // shotgun shell (MC_shotgun_shell, nose first) with a hot glow
                if (shellModel_) drawModel(shellModel_, Mat4::translation(r.pos) * Mat4::rotation(rotationBetween({0, 0, -1}, r.vel)) * Mat4::scale({0.6f, 0.6f, 0.6f}), false);
                if (boltModel_) drawModel(boltModel_, Mat4::translation(r.pos) * Mat4::scale({1.0f, 1.0f, 1.0f}), true, 0.7f);
                continue;
            }
            // The rocket model points along -Y (game) = -Z (runtime): orient it along its flight.
            const Vec3 f = normalize(r.vel);
            const Quat yawQ = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
            const Quat look = yawQ * Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, f.y))));
            drawModel(rocketModel_, Mat4::translation(r.pos) * Mat4::rotation(look) * Mat4::scale({0.6f, 0.6f, 0.6f}), false);
        }
    for (const auto& c : chars_) {
        if (!c.visible || length(c.pos - v.eyePosition) > 160.0f) continue;
        if (cut && std::find(cutTypes.begin(), cutTypes.end(), c.type) != cutTypes.end()) continue;
        c.model->render(renderer_, c.xf());
    }
    renderBurrows(v.eyePosition);
    renderShop();
    renderBase(v.eyePosition);
    renderPopup();
    renderGrenades();
    renderMines();
    renderSkirmish(false, v.eyePosition);
    renderSpells();
    renderKabutoPowers(v.eyePosition);
    renderThrows();
    renderMap();
    for (const auto& sp : creatures_.spits) {
        const int k = int(sp.size);
        if (k > 0 && spitBall_[k])
            spitBall_[k]->renderGlow(renderer_, Mat4::translation(sp.pos - spitCentre_[k]) *
                                                    Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, sp.age * 3.0f)), 1.3f);
    }
    for (const auto& b : blasts_) {
        // Burst: the flash plus the splash ring (ripspit / ripspitbig) growing and fading.
        const float f = b.age / 0.6f;
        const int k = b.size <= 2 ? b.size : 0;  // 3 = a Ripper bursting: flash only, no spit splash ring
        const float big = b.size >= 2 ? 1.5f : 1.0f;
        if (impactModel_)
            drawModel(impactModel_, Mat4::translation(b.pos) * Mat4::rotation(headWorldRot_) *
                                        Mat4::scale(Vec3{1, 1, 1} * big * (4.0f + 8.0f * f)), true, 1.0f - f);
        if (k > 0 && spitRing_[k])
            drawModel(spitRing_[k], Mat4::translation(b.pos + Vec3{0, 0.1f, 0}) * Mat4::scale(Vec3{1, 1, 1} * (2.0f + 10.0f * f) * big),
                      true, 1.0f - f);
    }
    for (const auto& f : waterFx_) {
        SkinnedModel* m = f.kind == 0 ? splashFx_.get() : ringFx_.get();
        if (!m) continue;
        if (f.kind == 0) {
            // The splash crown (its clip doesn't move it): opens out, leaps up and falls back in.
            const float k = f.age / 0.9f;
            if (k >= 1.0f) continue;
            const float wide = f.scale * (0.45f + 0.85f * std::sqrt(k));
            const float tall = f.scale * 1.25f * std::sin(3.14159f * std::min(1.0f, k * 1.1f));
            if (tall < 0.01f) continue;
            // Not with the eye inside the crown (a full view of translucent sheets: the jet-ski spray).
            if (length(Vec3{f.pos.x - v.eyePosition.x, 0, f.pos.z - v.eyePosition.z}) < wide * 1.3f && v.eyePosition.y < f.pos.y + tall * 1.5f) continue;
            m->pose(renderer_, 0.0f);
            const float spin = std::fmod(f.pos.x * 7.3f + f.pos.z * 3.1f, 6.28f);
            m->render(renderer_, Mat4::translation(f.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, spin + k * 0.6f)) *
                                     Mat4::scale(Vec3{wide, tall, wide}));
            continue;
        }
        m->pose(renderer_, std::min(f.age, m->animDuration() * 0.999f));
        if (length(f.pos - v.eyePosition) < 0.6f * f.scale) continue;  // (a ring seen from inside its own spray)
        m->render(renderer_, Mat4::translation(f.pos) * Mat4::scale(Vec3{1, 1, 1} * f.scale));
    }
    if (pirSwim_) {
        // Circling just under the surface; one now and then leaps (pir_swim_jump) at the prey.
        pirSwim_->pose(renderer_, float(time_));
        for (const auto& sc : schools_) {
            if (sc.vis <= 0.01f || length(sc.centre - v.eyePosition) > 120.0f) continue;
            for (int i = 0; i < kPiranhas; ++i) {
                const Piranha& f = sc.fish[i];
                const float r = (sc.target < 0 ? 1.2f : 0.8f) + 0.35f * float(i % 3);
                const Vec3 at = sc.centre + Vec3{std::cos(f.angle) * r, 0, std::sin(f.angle) * r};
                const float yaw = std::atan2(std::sin(f.angle), -std::cos(f.angle));  // swimming along the circle
                const float depth = -0.25f - (1.0f - sc.vis) * 1.5f;
                if (f.jump >= 0 && pirJump_) {
                    pirJump_->pose(renderer_, f.jump);
                    const Vec3 to = sc.centre - at;
                    pirJump_->render(renderer_, Mat4::translation(Vec3{at.x, depth, at.z}) *
                                                    Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, std::atan2(-to.x, -to.z))));
                } else {
                    pirSwim_->render(renderer_, Mat4::translation(Vec3{at.x, depth, at.z}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yaw)));
                }
            }
        }
    }
    renderGore();
    for (const auto& c : collapses_)
        if (c.model) c.model->render(renderer_, c.xf);
    renderPickups();
    renderHealthBars();
    renderVillagers(false, renderer_.currentView().eyePosition, 90.0f);
    renderVimps(false, renderer_.currentView().eyePosition, 150.0f);
    renderJetski(false, renderer_.currentView().eyePosition);
    for (const auto& g : gibs_)
        drawModel(g.model, Mat4::translation(g.pos) * Mat4::rotation(Quat::fromAxisAngle(g.axis, g.angle)) *
                               Mat4::scale({g.scale, g.scale, g.scale}),
                  false);
    for (const auto& e : enemies_.enemies)
        if (e.slot >= 0)
            enemyPool_[size_t(e.slot)]->render(renderer_, Mat4::translation(e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0}) *
                                                              Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, e.yaw)));
    renderReaperWeapons(false, renderer_.currentView().eyePosition, 150.0f);
    // Reaper bolts: the spark in its own white (as the original's sea Reapermen fire).
    for (const auto& p : enemyFire_.projectiles()) {
        const Quat r = rotationBetween({0, 0, -1}, p.vel);
        for (const auto& part : boltModel_ ? boltModel_->parts : std::vector<RenderModel::Part>{}) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.blend = BlendMode::Additive;
            dp.unlit = true;
            dp.depthWrite = false;
            dp.doubleSided = true;
            dp.tint = {2.2f, 2.2f, 2.2f, 1.0f};
            renderer_.draw(part.mesh, Mat4::translation(p.pos) * Mat4::rotation(r) * Mat4::scale({0.6f, 0.6f, 0.6f}), dp);
        }
    }
    for (const auto& e : enemyFire_.effects())
        if (e.kind == WeaponEffect::Impact)
            drawModel(impactModel_, Mat4::translation(e.pos) * Mat4::rotation(headWorldRot_) *
                                        Mat4::scale(Vec3{1, 1, 1} * (2.0f + 3.0f * e.progress())), true, 1.0f - e.progress());
    // The Meccaryn's pistol: small pink bolts (the original's), the spark quads facing along the
    // flight with a short fading trail.
    for (const auto& p : weapon_.projectiles()) {
        const Quat r = rotationBetween({0, 0, -1}, p.vel);
        if (p.style == 2 && sniperShotModel_) {  // the sniper's round: its own model, stretched along its flight
            drawModel(sniperShotModel_, Mat4::translation(p.pos) * Mat4::rotation(r) * Mat4::scale({0.6f, 0.6f, 2.5f}), true, 1.0f);
            continue;
        }
        if (p.style == 3 && arrowModel_) {  // an arrow: the rparrow streak
            drawModel(arrowModel_, Mat4::translation(p.pos) * Mat4::rotation(r) * Mat4::scale({0.2f, 0.2f, 0.2f}), true, 0.9f);
            continue;
        }
        const Vec3 back = normalize(p.vel) * -0.22f;
        const bool mg = p.style == 1;  // machine-gun tracers: smaller and hotter
        for (int i = 0; i < 3; ++i) {
            const float s = (0.32f - 0.07f * float(i)) * (mg ? 0.7f : 1.0f);
            if (p.travelled < 0.22f * float(i)) break;  // no trail behind the muzzle
            const float a = 2.6f - 0.8f * float(i);
            for (const auto& part : boltModel_ ? boltModel_->parts : std::vector<RenderModel::Part>{}) {
                DrawParams dp;
                dp.texture = part.texture;
                dp.blend = BlendMode::Additive;
                dp.unlit = true;
                dp.depthWrite = false;
                dp.doubleSided = true;
                dp.tint = mg ? Vec4{1.0f * a, 0.7f * a, 0.25f * a, 1.0f} : Vec4{1.0f * a, 0.35f * a, 0.75f * a, 1.0f};
                renderer_.draw(part.mesh, Mat4::translation(p.pos + back * float(i)) * Mat4::rotation(r) * Mat4::scale({s, s, s}), dp);
            }
        }
    }
    // Third-person crosshair: original bracket texture at the aim point, constant angular size.
    if (playMode_ && !firstPerson_ && crosshairTex_ && impactModel_ && lastInput_.hand[rig_.turnHand()].aimPoseValid) {
        const Vec3 eye = renderer_.currentView().eyePosition;
        const Vec3 toEye = eye - aimTarget_;
        const float dist = length(toEye);
        const float s = std::max(0.45f, dist * 0.33f);  // impact quad is 0.2 m: ~3.8 deg
        const Vec3 p = aimTarget_ + toEye * (std::min(0.5f, dist * 0.05f) / std::max(dist, 1e-3f));
        for (const auto& part : impactModel_->parts) {
            DrawParams dp;
            dp.texture = crosshairTex_;
            dp.blend = BlendMode::AlphaBlend;
            dp.unlit = true;
            dp.depthWrite = false;
            dp.doubleSided = true;
            dp.noFog = true;
            renderer_.draw(part.mesh, Mat4::translation(p) * Mat4::rotation(headWorldRot_) * Mat4::scale({s, s, s}), dp);
        }
    }
    for (const auto& e : weapon_.effects()) {
        const float k = e.progress();
        if (e.kind == WeaponEffect::Muzzle && e.style == 2 && sniperFlashModel_)
            drawModel(sniperFlashModel_, Mat4::translation(e.pos) * Mat4::rotation(rotationBetween({0, 0, -1}, e.dir)) *
                                             Mat4::scale(Vec3{1, 1, 1} * (0.5f + 0.4f * k)), true, 1.0f - k);
        else if (e.kind == WeaponEffect::Muzzle)
            drawModel(muzzleModel_, Mat4::translation(e.pos) * Mat4::rotation(rotationBetween({0, 0, -1}, e.dir)) *
                                        Mat4::scale(Vec3{1, 1, 1} * (0.25f + 0.15f * k)), true, 1.0f - k);
        else  // impact flash: billboard toward the viewer
            drawModel(impactModel_, Mat4::translation(e.pos) * Mat4::rotation(headWorldRot_) *
                                        Mat4::scale(Vec3{1, 1, 1} * (3.0f + 4.0f * k)), true, 1.0f - k);
    }
    renderFx();
    renderDataFx();
    if (title_) renderTitle();
    beginUiFrame(eyes, n);  // the HUD, menu and hands at human scale (Kabuto)
    if (cut) renderCutscene();
    else if (playMode_) renderHud();
    if (playMode_ && !cut) renderTags();
    if (playMode_ && !cut) renderTimmyLine();
    if (playMode_ && !cut) renderSkirmishHud();
    if (playMode_ && !cut) renderRaceHud();
    if (briefHeld_ && !cut && playMode_) renderBriefOverlay();
    if (menuOpen_ && !cut) renderMenu();
    if (showTestScene_ || !hasOriginalAssets()) testScene_.render(renderer_, rig_, lastInput_);
    else if (menuOpen_ || title_ || !playMode_ || briefHeld_) testScene_.renderPointers(renderer_, rig_, lastInput_);  // (in play: your hero's own hands)
    if (load_.phase == LoadPhase::FadeOut) renderLoading();
    renderVignette();
    endUiFrame();
    // Death: fade to black before the respawn, and back in after it.
    const float black = dying_ ? std::min(1.0f, std::max(0.0f, (deathTime_ - 4.3f) / 1.0f)) : respawnFade_;
    if (black > 0.01f && text_.ready()) {
        const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
        text_.drawPanel(Mat4::translation(v.eyePosition + fwd * 0.1f) * Mat4::rotation(headWorldRot_), 1.0f, 1.0f, {0, 0, 0, black});
    }
    renderer_.endView();
}

void GameApp::shutdown() {
    playerModel_.reset();
    assetWorld_.reset();
    renderer_.shutdown();
}

}  // namespace gvr
