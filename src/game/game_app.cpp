#include "game/game_app.h"

#include "core/log.h"
#include "scene/asset_world.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "assets/gtext.h"
#include "game/cutscene.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
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
    shadowsOn_ = renderer_.initShadows(1024);
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
        // Two variants of the same Meccaryn: with the jetpack on his back (mc_l0) and without
        // (mc_nothrust_l0, same skeleton and clips); the one in use follows whether the player has
        // the jetpack (1a: Baz lost it, Timmy hands it over).
        auto makePlayer = [this](const char* base, bool first) -> std::unique_ptr<SkinnedModel> {
        auto m = std::make_unique<SkinnedModel>();
        if (!m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, base, "mc_stand.anm")) return nullptr;
        {
            const char* clips[] = {"mc_stand.anm", "mc_run.anm", "mc_thrust.anm", "mc_hover.anm", "mc_land_soft.anm", "mc_sidestep.anm",
                                   "mc_skid.anm"};
            for (int i = 1; i < int(PlayerAnim::Count); ++i) clip_[i] = std::max(0, m->addAnimation(assetWorld_->fs(), clips[i]));
            // Eye point from the idle pose: at the front of the face (-Z), so looking down shows
            // the chest and arms instead of the inside of the collar armour.
            m->pose(renderer_, 0.0f);
            Vec3 hmn, hmx;
            if (first && m->boneVertexBounds("head", hmn, hmx))
                eyeModel_ = {0.5f * (hmn.x + hmx.x), hmn.y + 0.55f * (hmx.y - hmn.y), hmn.z - 0.04f};
            if (first) LOGI("Player eye point (model, m): %.2f %.2f %.2f", eyeModel_.x, eyeModel_.y, eyeModel_.z);
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

            playerModel_ = std::move(m);
            playMode_ = true;
            AssetWorld* w = assetWorld_.get();
            // Weapon models: gun (held, so dynamically lit), bolt, muzzle flash, impact flash.
            ModelCache* mc = assetWorld_->models();
            gunModel_ = mc->get("mc_gun_l0.gbs", "", /*dynamicLit=*/true);
            launcherModel_ = mc->get("mc_launcher_l0.gbs", "", /*dynamicLit=*/true);
            rocketModel_ = mc->get("mc_rocket.gbs", "mc_rocket.anm", /*dynamicLit=*/true);
            boltModel_ = mc->get("mc_bullet.gbs", "mc_bullet.anm");
            muzzleModel_ = mc->get("muzzle_flash.gbs", "muzzle_flash.anm");
            impactModel_ = mc->get("efx_flash.gbs");
            crosshairTex_ = mc->texture("st_xhair_lock");  // original crosshair (red brackets)
            worldRay_ = [w](const Vec3& o, const Vec3& d, float maxD, float& t, Vec3& n) {
                float tc = 0;
                Vec3 nc;
                const bool hc = w->collision().raycast(o, d, maxD, tc, nc);
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
                if (hc && (!ht || tc < tt)) { t = tc; n = nc; return true; }
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
                const bool hw = worldRay_ && worldRay_(o, d, maxD, tw, nw);
                bool he = enemies_.raycast(o, d, maxD, te, ne, ie);
                float tc = maxD;
                Vec3 nc;
                int ic = -1;
                if (creatures_.raycast(o, d, maxD, tc, nc, ic) && (!he || tc < te)) {
                    he = true; te = tc; ne = nc; ie = kCreatureTarget + ic;
                }
                if (!hw && !he) return false;
                if (he && (!hw || te < tw)) { t = te; n = ne; target = ie; }
                else { t = tw; n = nw; target = -1; }
                return true;
            };
            weapon_.onHit = [this](int target, const Vec3& p, const Vec3& shotDir) {
                if (target < 0) {
                    sfx(frand() < 0.5f ? "bi_dirt1" : (frand() < 0.5f ? "bi_hit1" : "bi_hit2"), p, 0.45f, 0.9f + 0.2f * frand());
                    return;
                }
                if (target >= kCreatureTarget) {
                    std::vector<CreatureEvent> ev;
                    const int ci = target - kCreatureTarget;
                    const float scale = creatures_.creatures[size_t(ci)].scale;
                    const int slot = creatures_.creatures[size_t(ci)].slot;
                    const bool died = creatures_.damage(ci, 25.0f, shotDir, &ev);
                    if (died && slot >= 0 && size_t(slot) < chars_.size() && chars_[size_t(slot)].objective >= 0)
                        completeObjective(chars_[size_t(slot)].objective);
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
                const bool killed = enemies_.damage(target, 25.0f);
                if (killed && enemies_.enemies[size_t(target)].objective >= 0)
                    completeObjective(enemies_.enemies[size_t(target)].objective);
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
                const Vec3 a = player_.position + Vec3{0, 0.35f, 0}, b = player_.position + Vec3{0, 1.65f, 0};
                const bool hp = playMode_ && rayCapsule(o, d, maxD, a, b, 0.35f, tp);
                if (!hw && !hp) return false;
                if (hp && (!hw || tp < tw)) { t = tp; n = d * -1.0f; target = -2; }
                else { t = tw; n = nw; target = -1; }
                return true;
            };
            enemyFire_.onHit = [this](int target, const Vec3& p, const Vec3&) {
                if (target == -2) damagePlayer(5.0f);
                else sfx(frand() < 0.5f ? "bi_hit1" : "bi_dirt1", p, 0.35f, 0.9f + 0.2f * frand());
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
                                    "rpman_hit_spin_die.anm", "rpman_hit_gut_die.anm", "rpman_bzka_run.anm"};
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
            initMission();
            cutscene_ = std::make_unique<CutscenePlayer>(w->fs(), *mc, renderer_, w->registry());
            cutscene_->groundHeight = [w](float x, float z) {
                float y;
                return w->terrain().heightAt(x, z, y) ? y : -1e9f;
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
            player_.collision = &w->collision();
            // Off the terrain heightfield is open sea; the sea (surface at 0) holds the player up.
            player_.groundHeight = [w](float x, float z) {
                float y;
                return w->terrain().heightAt(x, z, y) ? y : -20.0f;
            };
            player_.seaLevel = 0.0f;
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
    // Camera collision: if a cliff or building is between the character and the viewer's eyes,
    // pull the camera in front of it so the character stays visible.
    const float eyeH = (in && in->headValid) ? in->head.position.y : 1.6f;
    const Vec3 chest = player_.position + Vec3{0, 1.4f, 0};
    const Vec3 eye = pos + Vec3{0, eyeH, 0};
    const Vec3 toEye = eye - chest;
    const float dist = length(toEye);
    float t;
    Vec3 n;
    if (worldRay_ && dist > 0.5f && worldRay_(chest, toEye * (1.0f / dist), dist, t, n)) {
        const Vec3 newEye = chest + toEye * (std::max(0.3f, t - 0.4f) / dist);
        pos = newEye - Vec3{0, eyeH, 0};
    }
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
    // The jetpack on his back only once he has it.
    if (playerSpare_ && playerHasJet_ != player_.jetpackAvailable) {
        std::swap(playerModel_, playerSpare_);
        playerHasJet_ = !playerHasJet_;
        LOGI("Player model: %s jetpack", playerHasJet_ ? "with" : "without");
    }
    PlayerInput pi;
    pi.move = in.hand[mh].thumbstick;
    pi.viewYaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : camYaw_;
    pi.jet = in.hand[th].primary;              // A
    pi.sprint = in.hand[mh].squeeze > 0.5f;    // move-hand grip
    if (!firstPerson_) {
        updateThirdPersonAim(in, dt);
        pi.aiming = tpAiming_;
        const Vec3 d = aimTarget_ - player_.position;
        pi.aimYaw = std::atan2(-d.x, -d.z);
    } else {
        tpAiming_ = false;
    }
    player_.update(dt, pi);
    placeCamera(dt, false, &in);
    if (!playerModel_) return;
    if (firstPerson_) {
        poseFirstPersonBody(in);
    } else {
        bodyXf_ = Mat4::translation(player_.position) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, player_.yaw));
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
    playerModel_->poseBlend(renderer_, clip_[int(player_.prevAnim())], player_.prevAnimTime(),
                            clip_[int(player_.anim())], player_.animTime(), player_.fadeWeight());
    eyeNow_ = playerModel_->hiddenCentroid() + eyeFromHead_;
    if (in.headValid) headWorldRot_ = rig_.toWorld(in.head).orientation;
    updateWeapon(dt, in);
    updateRescue(dt, in);
    updateCombat(dt);
    updateAudio(dt);
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
                haptics_.push_back({kLeft, 0.3f, 0.05f});
                haptics_.push_back({kRight, 0.3f, 0.05f});
                completeObjective(s.objective);
                LOGI("Smartie %d saved (%zu/%zu)", e.index, rescue_.savedCount(), rescue_.smarties.size());
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
                waterFx_.push_back({Vec3{e.pos.x, 0.02f, e.pos.z}, 0.0f, 0, 0.8f});
                sfx("se_splsh", e.pos, 0.9f);
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
    if (!audio_ || !assetWorld_) return;
    static const char* names[] = {
        "me_peas1", "me_peas2", "bi_dirt1", "bi_hit1", "bi_hit2", "ex_bdys1", "rh_sprsh", "rh_fsld1",
        "rpman_hit1", "rpman_hit2", "rpman_hit3", "rpman_hit4", "rpman_die1", "rpman_die2", "rpman_die3",
        "rpman_die4", "rpman_die5", "me_shrt1", "me_shrt2", "me_shrt3", "me_jpkst", "me_jpklp", "me_jpken",
        "me_jpkno", "me_fsgr1", "me_fsgr2", "me_hdfl1", "me_hdfl2", "me_sld1", "me_slden",
        // Rippers: rs_ small, rm_ medium, rl_ large (inferred from the names and the three Ripper types).
        "rs_attk1", "rs_attk2", "rs_chitt", "rs_deat1", "rs_deat2", "rs_feet", "rs_injr1", "rs_injr2",
        "rm_atfr1", "rm_atfr2", "rm_attk1", "rm_chitt", "rm_deat1", "rm_deat2", "rm_feet", "rm_frexp", "rm_inj01",
        "rm_inj02", "rl_atfr1", "rl_atfr2", "rl_attk1", "rl_chitt", "rl_deat1", "rl_deat2", "rl_feet", "rl_frexp1",
        "rl_frexp2", "rl_injr1", "rl_injr2", "ex_bdyl1", "ex_bdyl2",
        // Carrying Smarties (1b): pick up / let go / drop off, splash, piranhas.
        "me_smton", "me_smtof", "me_smtdp", "se_splsh", "pi_swrm1", "pi_swrm2",
        // Rocket launcher: launch, explosion.
        "me_msls1", "me_mslex"};
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
    if (player_.onGround && (pa == PlayerAnim::Run || pa == PlayerAnim::Strafe) && playerModel_) {
        const float dur = std::max(0.1f, playerModel_->animDuration(clip_[int(pa)]));
        // Half-cycle counter: a footstep each time the clip enters a new half (either direction).
        const float halfIndex = std::floor(player_.animTime() / dur * 2.0f);
        if (halfIndex != prevRunPhase_) sfx2D(int(halfIndex) & 1 ? "me_fsgr2" : "me_fsgr1", 0.3f, 0.95f + 0.1f * frand());
        prevRunPhase_ = halfIndex;
    }
    // Reaper footsteps while running.
    if (slotStep_.size() != enemyPool_.size()) slotStep_.assign(enemyPool_.size(), 0.0f);
    for (const auto& e : enemies_.enemies) {
        if (e.slot < 0 || e.clip != EnemyClip::Run || e.state == EnemyState::Dead) continue;
        float& t = slotStep_[size_t(e.slot)];
        t -= dt;
        if (t <= 0) {
            t = 0.3f;
            if (length(e.pos - player_.position) < 30.0f) sfx("rh_fsld1", e.pos, 0.5f, 0.9f + 0.2f * frand());
        }
    }
}

void GameApp::initCharacters(AssetWorld* w, ModelCache* mc) {
    const ObjectRegistry& reg = w->registry();
    size_t reapers = 0, rippers = 0, ambient = 0;
    for (const auto& ch : w->characters()) {
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
        auto m = std::make_unique<SkinnedModel>();
        if (idle.empty() || !m->load(w->fs(), *mc, renderer_, ch.model, idle)) continue;
        CharacterInst ci;
        ci.type = int(ch.type);
        ci.pos = ch.position;
        ci.yaw = ch.yaw;
        ci.scale = ch.scale;
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
            // 44 hit, 49 small hit, 45 suffer (ends lying on the ground: the collapse death), 50 spit.
            static const int slots[int(CreatureClip::Count)] = {1, 2, 10, 11, 44, 49, 45, 50};
            for (int c = 1; c < int(CreatureClip::Count); ++c) {
                const std::string a = reg.anim(ch.type, slots[c]);
                const int k = a.empty() ? -1 : m->addAnimation(w->fs(), a);
                ci.clipIndex[c] = k >= 0 ? k : 0;
            }
            if (rippers == 0)
                for (int c = 0; c < int(CreatureClip::Count); ++c)
                    creatures_.clipDuration[c] = std::max(0.1f, m->animDuration(ci.clipIndex[c]));
            ci.ambient = false;
            // Three Ripper types share the model; their type records carry hit points 20 / 40 / 90
            // (type op 0x31) and the data has three sound sets (rs_/rm_/rl_) and two spit balls.
            const ObjectType* t = reg.type(ch.type);
            const float hp = t && t->hitPoints > 0 ? t->hitPoints : 40.0f;
            ci.size = hp <= 25.0f ? CreatureSize::Small : (hp <= 60.0f ? CreatureSize::Medium : CreatureSize::Large);
            const int idx = creatures_.spawn(ch.position, ch.yaw, ch.scale, ci.size, hp, true);
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
    rescue_.groundHeight = enemies_.groundHeight;
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
    LOGI("Mission characters: %zu Rippers, %zu Reapers, %zu ambient", rippers, reapers, ambient);
}

void GameApp::spawnGore(const Vec3& pos, const Vec3& dir, float scale) {
    // The original gore pieces (rip_gore_head, rip_gore_arm x2) thrown along the killing shot.
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
            play("deat1", "deat2", 1.0f);
            if (e.burst) sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", e.pos, 0.9f, 0.95f + 0.1f * frand());
            break;
    }
}

void GameApp::updateCharacters(float dt) {
    creatures_.update(dt, player_.position, playMode_ && playerHealth_ > 0, creatureEvents_);
    for (const auto& e : creatureEvents_) {
        creatureSound(e);
        if (e.kind == CreatureEvent::SpitLaunch) ++spitsLaunched_;
        if (e.kind == CreatureEvent::SpitBurst && e.damage > 0.5f) ++spitHits_;
        if ((e.kind == CreatureEvent::Strike || e.kind == CreatureEvent::SpitBurst) && e.damage > 0.5f)
            damagePlayer(e.damage);
        if (e.kind == CreatureEvent::SpitBurst) blasts_.push_back({e.pos, int(e.size), 0.0f});
    }
    // Rippers: the model follows its creature; burst or sunk-away bodies are hidden.
    for (auto& ci : chars_)
        if (!ci.ambient) ci.visible = false;
    for (auto& c : creatures_.creatures) {
        if (c.slot < 0 || size_t(c.slot) >= chars_.size() || c.exploded) continue;
        CharacterInst& ci = chars_[size_t(c.slot)];
        ci.visible = true;
        ci.pos = c.pos;
        ci.yaw = c.yaw;
        const float dist = length(c.pos - player_.position);
        if (dist > 120.0f) continue;  // too far to see the animation: skip skinning
        ci.model->poseBlend(renderer_, ci.clipIndex[int(c.prevClip)], c.prevTime, ci.clipIndex[int(c.clip)], c.clipTime,
                            c.fadeWeight(creatures_.params.crossfade));
        Vec3 mn, mx;
        ci.model->posedBounds(mn, mx);
        ci.lift += (-mn.y * ci.scale - ci.lift) * std::min(1.0f, dt / 0.06f);
        // Footsteps while running.
        if (c.clip == CreatureClip::Walk && c.speed > 0.5f && dist < 30.0f) {
            ci.stepTimer -= dt * c.rate;
            if (ci.stepTimer <= 0) {
                ci.stepTimer = 0.45f;
                const char* feet[3] = {"rs_feet", "rm_feet", "rl_feet"};
                sfx(feet[int(ci.size)], c.pos, 0.35f, 0.9f + 0.2f * frand());
            }
        }
    }
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
        if (g.life > 6.0f) {  // sink away
            g.pos.y -= 0.4f * dt;
            continue;
        }
        if (g.pos.y < ground + 0.1f * g.scale) {  // bounce, then settle
            g.pos.y = ground + 0.1f * g.scale;
            g.vel = Vec3{g.vel.x * 0.5f, std::fabs(g.vel.y) * 0.3f, g.vel.z * 0.5f};
            g.spin *= 0.5f;
        }
    }
    gibs_.erase(std::remove_if(gibs_.begin(), gibs_.end(), [](const Gib& g) { return g.life > 8.0f; }), gibs_.end());
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
        if (!ci.ambient) continue;
        ci.time += dt;
        if (length(ci.pos - player_.position) < 120.0f) ci.model->pose(renderer_, ci.time);
    }
}

void GameApp::initMission() {
    // Proximity triggers on placed objects (death triggers were added with their characters).
    for (const auto& t : assetWorld_->triggerObjects())
        if (t.kind == 1) objectives_.push_back({1, t.boundsMin, t.boundsMax});
    // Starting inventory: a Mecc mission lists the items the player must hold (op 0x0E). Item 1
    // is read as the jetpack: absent only in 1a_1_timmy, where Baz loses it in the opening
    // cutscene and Timmy hands it over (provisional reading, see the engineering log).
    const MissionData& m = assetWorld_->mission();
    bool jet = true;
    if (m.setup[0] == 2) jet = std::find(m.items.begin(), m.items.end(), 1u) != m.items.end();
    player_.jetpackAvailable = jet;
    // Item 3 is read as the rocket launcher ("RPG"): first listed in 1c, after Timmy hands it over
    // at the end of 1b (provisional, like item 1 = jetpack).
    if (m.setup[0] == 2 && std::find(m.items.begin(), m.items.end(), 3u) != m.items.end() && launcherModel_) hasLauncher_ = true;
    if (std::getenv("GVR_LAUNCHER") && launcherModel_) hasLauncher_ = true;  // test hook
    if (!hasLauncher_) weaponSel_ = 0;
    LOGI("Mission %s: %zu objectives, jetpack %s", assetWorld_->missionName().c_str(), objectives_.size(),
         jet ? "available" : "not yet (story)");
}

void GameApp::updateSun(float dt) {
    // The sun circles the sky once in 20 minutes, between 30 and 62 degrees up: it never sets,
    // and the shadows swing round with it. (Ours: the original's sun is fixed per level.)
    EnvironmentParams& env = renderer_.environment();
    env.time = float(time_);
    sunAzimuth_ += dt * 2.0f * kPi / 1200.0f;
    const float el = (46.0f + 16.0f * std::sin(sunAzimuth_ * 2.0f)) * kPi / 180.0f;
    const Vec3 toSun{std::cos(el) * std::sin(sunAzimuth_), std::sin(el), std::cos(el) * std::cos(sunAzimuth_)};
    env.sunDirection = toSun * -1.0f;
}

void GameApp::renderShadows() {
    if (!shadowsOn_ || !assetWorld_ || !assetWorld_->loaded()) { renderer_.setShadowsActive(false); return; }
    const Vec3 centre = playMode_ ? player_.position : rig_.worldFromTracking.position;
    const float half = 28.0f;  // 56 m square around the player, ~5.5 cm texels at 1024
    renderer_.beginShadowPass(renderer_.sunShadowMatrix(centre, half));
    assetWorld_->renderShadowCasters(renderer_, centre, half * 1.5f);
    const bool cut = cutsceneActive();
    for (const auto& c : chars_) {
        if (!c.visible || length(c.pos - centre) > half * 1.4f) continue;
        c.model->renderShadow(renderer_, Mat4::translation(c.pos + Vec3{0, c.lift, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, c.yaw)) *
                                             Mat4::scale({c.scale, c.scale, c.scale}));
    }
    for (const auto& e : enemies_.enemies)
        if (e.slot >= 0 && length(e.pos - centre) < half * 1.4f)
            enemyPool_[size_t(e.slot)]->renderShadow(renderer_, Mat4::translation(e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0}) *
                                                                    Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, e.yaw)));
    // The player's own body (third person; in first person his head is hidden, which would show).
    if (playMode_ && playerModel_ && !cut && !firstPerson_) playerModel_->renderShadow(renderer_, bodyXf_);
    renderer_.endShadowPass();
    renderer_.setShadowsActive(true);
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

void GameApp::beginLoading(const std::string& mission, const InputFrame& in, bool load) {
    load_ = LoadState{};
    load_.phase = load ? LoadPhase::FadeOut : LoadPhase::Brief;
    load_.mission = load ? mission : std::string();
    // The briefing faces where the player looks now (head yaw), 2 m away.
    load_.yaw = in.headValid ? yawOf(rig_.toWorld(in.head).orientation) : camYaw_;
    load_.title = mission;
    load_.brief.clear();
    if (gameText_) {
        const std::string t = gameText_->get(mission);
        if (!t.empty()) load_.title = t;
        // Objectives text: '^' = new line, '~N' = colour change (dropped).
        const std::string mo = gameText_->get("MO_" + mission);
        for (size_t i = 0; i < mo.size(); ++i) {
            if (mo[i] == '^') load_.brief += '\n';
            else if (mo[i] == '~' && i + 1 < mo.size()) ++i;
            else load_.brief += mo[i];
        }
        while (!load_.brief.empty() && (load_.brief.front() == '\n' || load_.brief.front() == ' ')) load_.brief.erase(0, 1);
        while (!load_.brief.empty() && (load_.brief.back() == '\n' || load_.brief.back() == ' ')) load_.brief.pop_back();
    }
    LOGI("Loading %s (%s)", mission.c_str(), load_.title.c_str());
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
                if (!load_.mission.empty()) loadMission(load_.mission);
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
    auto place = [&](float up) { return Mat4::translation(base + Vec3{0, up, 0}) * Mat4::rotation(r); };
    float w = 0, h = 0;
    if (const MeshId t = text_.mesh(load_.title, 0.14f, 1.8f, &w, &h)) text_.draw(t, place(0.55f), {1.0f, 0.85f, 0.4f, 1.0f});
    if (!load_.brief.empty())
        if (const MeshId b = text_.mesh(load_.brief, 0.055f, 2.6f, &w, &h)) text_.draw(b, place(0.1f - 0.5f * h + 0.3f), {0.9f, 0.9f, 0.85f, 1.0f});
    const float pulse = 0.55f + 0.45f * std::sin(load_.t * 4.0f);
    const char* label = load_.phase == LoadPhase::Show ? "LOADING..." : "GET READY";
    if (const MeshId l = text_.mesh(label, 0.07f, 1.5f, &w, &h)) text_.draw(l, place(-0.75f), {1, 1, 1, pulse});
}

void GameApp::loadMission(const std::string& name) {
    AssetWorld* w = assetWorld_.get();
    if (!w || !w->selectMission(name)) { LOGW("Mission %s unavailable", name.c_str()); return; }
    chars_.clear();
    creatures_.creatures.clear();
    creatures_.spits.clear();
    enemies_.enemies.clear();
    rescue_ = RescueDirector{};
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
    LOGI("Mission %s loaded", name.c_str());
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
    // The outro cutscene (op 0x19) isn't played yet; its soundtrack carries the dialogue. In
    // 1a_1_timmy that is Timmy handing Baz the jetpack.
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
        if (length(c - p) <= 10.0f * gGameUnitsToMeters) completeObjective(int(i));
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
    const int race = int(assetWorld_->mission().setup[0]);
    ambientSet_ = race == 1 ? "ka1" : (race == 3 ? "ra1" : "ma1");
    music_->request(ambientSet_);
    audio_->setMusicVolume(0.8f);
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
    if (combatHold_ > 0) want = pre + (engaged >= 4 ? "lb" : "sb");
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

void GameApp::updateCutscene(float dt, const InputFrame& in) {
    cutscene_->update(dt);
    const std::string stream = cutscene_->takeStream();
    if (!stream.empty()) playStream(stream);
    for (const auto& c : cutscene_->takeCues()) playStream(c, true);
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
    } else if (cutscene_->active()) {
        // Travel with a moving shot camera (translation only; the view never rotates by itself).
        const Vec3 now = aboveGround(cutscene_->eyeNow());
        rig_.worldFromTracking.position = rig_.worldFromTracking.position + (now - cutEye_);
        cutEye_ = now;
    }
    if (!cutscene_->active()) {
        // Back to the game where the player stands.
        if (playMode_) rig_.teleportHead(in, player_.position, player_.yaw);
        LOGI("Cutscene finished");
    }
}

void GameApp::renderCutscene() {
    cutscene_->render();
    const Vec3 eye = renderer_.currentView().eyePosition;
    // Subtitles: a panel below the line of sight, fixed for the shot.
    if (gameText_ && text_.ready()) {
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
            const Quat r = Quat::fromAxisAngle({0, 1, 0}, cutYaw_);
            const Vec3 at = cutEye_ + r.rotate(Vec3{0, -0.38f, -1.5f});
            const Mat4 xf = Mat4::translation(at) * Mat4::rotation(r * Quat::fromAxisAngle({1, 0, 0}, 0.25f));
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

void GameApp::damagePlayer(float amount) {
    if (playerHealth_ <= 0) return;
    playerHealth_ -= amount;
    sinceDamage_ = 0;
    damageFlash_ = 1.0f;
    if (hurtCooldown_ <= 0) {
        static const char* hurt[] = {"me_shrt1", "me_shrt2", "me_shrt3"};
        sfx2D(hurt[int(frand() * 2.99f)], 0.8f);
        hurtCooldown_ = 0.6f;
    }
    haptics_.push_back({kLeft, 0.7f, 0.1f});
    haptics_.push_back({kRight, 0.7f, 0.1f});
    if (playerHealth_ <= 0) {
        LOGI("Player died: respawning");
        playerHealth_ = 100.0f;
        teleportPlayer(assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
        damageFlash_ = 2.0f;
    }
}

void GameApp::explode(const Vec3& at, float radius, float damage) {
    blasts_.push_back({at, 3, 0.0f});
    sfx("me_mslex", at, 1.0f, 0.95f + 0.1f * frand());
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Enemy& e = enemies_.enemies[i];
        if (e.state == EnemyState::Dead) continue;
        const float d = length(e.pos + Vec3{0, 0.9f, 0} - at);
        if (d > radius) continue;
        if (enemies_.damage(int(i), damage * (1.0f - 0.6f * d / radius)) && enemies_.enemies[i].objective >= 0)
            completeObjective(enemies_.enemies[i].objective);
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
    LOGI("Rocket exploded at (%.1f %.1f %.1f), %zu creatures alive", at.x, at.y, at.z, creatures_.aliveCount());
    // The player is not immune to their own rockets.
    const float dp = length(player_.position + Vec3{0, 0.9f, 0} - at);
    if (playMode_ && dp < radius * 0.6f) damagePlayer(30.0f * (1.0f - dp / (radius * 0.6f)));
}

void GameApp::updateRockets(float dt) {
    for (auto& r : rockets_) {
        r.life += dt;
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
            explode(hit ? r.pos + dir * t : r.pos, 4.0f, 120.0f);
            r.life = 1e9f;
            continue;
        }
        r.pos += r.vel * dt;
    }
    rockets_.erase(std::remove_if(rockets_.begin(), rockets_.end(), [](const Rocket& r) { return r.life >= 1e9f; }), rockets_.end());
}

void GameApp::updateCombat(float dt) {
    updateRockets(dt);
    // Water: entering it splashes; swimming leaves rings. Piranhas live in all deep water (the 1b
    // briefing warns of them): after a moment there they gather round and bite; leaving deep water
    // (shallows, the shore, the jetpack) sends them away. Rates and damage ours.
    const bool swim = playMode_ && player_.swimming();
    if (swim && !wasSwimming_) {
        waterFx_.push_back({Vec3{player_.position.x, 0.02f, player_.position.z}, 0.0f, 0, 1.2f});
        sfx("se_splsh", player_.position, 0.9f);
    }
    wasSwimming_ = swim;
    if (swim && player_.horizontalSpeed() > 0.4f) {
        ringTimer_ -= dt;
        if (ringTimer_ <= 0) {
            ringTimer_ = 0.5f;
            waterFx_.push_back({Vec3{player_.position.x, 0.03f, player_.position.z}, 0.0f, 1, 2.5f});
        }
    }
    for (auto& f : waterFx_) f.age += dt;
    waterFx_.erase(std::remove_if(waterFx_.begin(), waterFx_.end(), [](const WaterFx& f) { return f.age > 2.0f; }), waterFx_.end());
    if (swim) {
        waterTime_ += dt;
        if (waterTime_ > 0.8f) {
            if (!piranhaAttack_) {
                piranhaAttack_ = true;
                for (int i = 0; i < kPiranhas; ++i) piranha_[i] = Piranha{float(i) * 1.2566f + frand(), -1.0f};
                LOGI("Piranhas!");
            }
            biteTimer_ -= dt;
            if (biteTimer_ <= 0) {
                biteTimer_ = 0.5f;
                sfx(frand() < 0.5f ? "pi_swrm1" : "pi_swrm2", player_.position, 1.0f, 0.9f + 0.2f * frand());
                damagePlayer(8.0f);
            }
        }
    } else {
        waterTime_ = 0;
        biteTimer_ = 0;
        piranhaAttack_ = false;
    }
    // The school follows the player while attacking and fades (dives) away otherwise.
    piranhaVis_ += ((piranhaAttack_ ? 1.0f : 0.0f) - piranhaVis_) * std::min(1.0f, dt * 2.5f);
    if (piranhaVis_ > 0.01f) {
        if (piranhaAttack_) piranhaCentre_ = player_.position;
        for (int i = 0; i < kPiranhas; ++i) {
            Piranha& f = piranha_[i];
            f.angle += dt * (1.6f + 0.25f * float(i));
            if (f.jump >= 0) f.jump += dt;
            if (f.jump > 2.3f) f.jump = -1;
            if (f.jump < 0 && piranhaAttack_ && frand() < dt * 0.25f) f.jump = 0;  // leaps at the player now and then
        }
    }
    // Health regenerates after 4 s without damage.
    sinceDamage_ += dt;
    if (sinceDamage_ > 4.0f) playerHealth_ = std::min(100.0f, playerHealth_ + 12.0f * dt);
    damageFlash_ = std::max(0.0f, damageFlash_ - dt * 3.0f);

    enemies_.update(dt, player_.position, playMode_, shooters_);
    // Model slots follow the enemies (the director removes sunk corpses).
    std::fill(slotUsed_.begin(), slotUsed_.end(), 0);
    for (auto& e : enemies_.enemies)
        if (e.slot >= 0 && size_t(e.slot) < slotUsed_.size()) slotUsed_[size_t(e.slot)] = 1;
    for (auto& e : enemies_.enemies)
        if (e.slot < 0)
            for (size_t k = 0; k < slotUsed_.size(); ++k)
                if (!slotUsed_[k]) { e.slot = int(k); slotUsed_[k] = 1; slotLift_[k] = enemyFootOffset_; break; }
    for (auto& e : enemies_.enemies) {
        if (e.slot < 0) continue;
        SkinnedModel& m = *enemyPool_[size_t(e.slot)];
        m.poseBlend(renderer_, int(e.prevClip), e.prevTime, int(e.clip), e.clipTime, e.fadeWeight(enemies_.params.crossfade));
        // Ground contact from the actual pose: the lowest point (planted foot, or the body lying
        // in a death pose) rests on the ground. Smoothed lightly against single-frame pops.
        Vec3 mn, mx;
        m.posedBounds(mn, mx);
        float& lift = slotLift_[size_t(e.slot)];
        lift += (-mn.y - lift) * std::min(1.0f, dt / 0.06f);
    }
    // Enemy shots from each Reaper's gun nozzle toward the player's chest, with spread.
    for (int i : shooters_) {
        const Enemy& e = enemies_.enemies[size_t(i)];
        const Quat r = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
        Vec3 muzzle = e.pos + Vec3{0, 1.5f, 0} + r.rotate(Vec3{0.3f, 0, -0.6f});
        Mat4 nz;
        if (e.slot >= 0 && enemyPool_[size_t(e.slot)]->nodeWorld("ref_nozzle", nz))
            muzzle = e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0} + r.rotate(giantsToRuntime(nz.m[12], nz.m[13], nz.m[14]));
        const Vec3 target = player_.position + Vec3{0, 1.2f, 0};
        Vec3 dir = normalize(target - muzzle);
        // Spread grows with distance: long shots mostly miss, close ones are dangerous.
        const float spread = 0.06f + 0.004f * length(target - muzzle);
        dir = normalize(dir + Vec3{frand() - 0.5f, (frand() - 0.5f) * 0.6f, frand() - 0.5f} * spread);
        enemyFire_.spawnShot(muzzle, dir);
        sfx("rh_sprsh", muzzle, 0.8f, 0.9f + 0.2f * frand());
    }
    enemyFire_.update(dt, false, Vec3{}, Vec3{0, 0, -1});
    // Next wave after the last Reaper has fallen.
    updateCharacters(dt);
    updateMission(dt);
    if (enemies_.aliveCount() == 0 && !missionHostiles_ && objectives_.empty()) {
        waveTimer_ += dt;
        if (waveTimer_ > 8.0f) { waveTimer_ = 0; spawnWave(); }
    } else {
        waveTimer_ = 0;
    }
}

// Gun muzzle, barrel direction and handle in the gun model's GAME frame (forward = -Y like
// the characters; up = +Z).
static const Vec3 kGunMuzzleGame{0.0f, -1.8f, 0.25f};
static const Vec3 kGunForwardGame{0.0f, -1.0f, 0.0f};
static const Vec3 kGunHandleGame{0.0f, 0.6f, -0.8f};

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

void GameApp::updateWeapon(float dt, const InputFrame& in) {
    const int gh = rig_.turnHand();
    // Right stick forward: next weapon (released to neutral before it switches again).
    const float sy = in.hand[gh].thumbstick.y;
    if (sy < 0.3f) switchArmed_ = true;
    if (switchArmed_ && sy > 0.75f && hasLauncher_) {
        switchArmed_ = false;
        weaponSel_ = 1 - weaponSel_;
        haptics_.push_back({gh, 0.5f, 0.06f});
        LOGI("Weapon: %s", weaponSel_ ? "rocket launcher" : "gun");
    }
    const bool launcher = weaponSel_ == 1 && launcherModel_;
    const RenderModel* held = launcher ? launcherModel_ : gunModel_;
    // Handle / muzzle in each model's GAME frame (forward -Y, up +Z): the launcher is a
    // shoulder tube (9.5 units long) held at its pistol grip, a little behind the middle.
    const Vec3 handleGame = launcher ? Vec3{0.0f, 1.0f, -1.2f} : kGunHandleGame;
    const Vec3 muzzleGame = launcher ? Vec3{0.0f, -4.7f, 0.1f} : kGunMuzzleGame;
    Mat4 ref;
    Vec3 muzzle, fwd;
    if (firstPerson_) {
        // First person: the gun follows the controller's aim pose exactly (shots go where you
        // point); handle at the grip, sized with the body's hands.
        const ControllerState& c = in.hand[gh];
        gunValid_ = held && c.aimPoseValid && c.gripPoseValid;
        if (gunValid_) {
            const Pose aim = rig_.toWorld(c.aimPose);
            const Pose grip = rig_.toWorld(c.gripPose);
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
            const Vec3 f = tpAiming_ ? normalize(aimTarget_ - hand) : normalize(body.rotate(Vec3{0, -0.5f, -1}));
            // Look rotation: -Z -> f, keeping +Y as close to world up as possible.
            const Quat yawQ = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
            const float pitch = std::asin(std::max(-1.0f, std::min(1.0f, f.y)));
            const Quat look = yawQ * Quat::fromAxisAngle({1, 0, 0}, pitch);
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
    }
    const bool trigger = in.hand[gh].trigger > 0.6f;
    rocketCooldown_ -= dt;
    if (launcher) {
        // One rocket per pull, 1 s to reload. The gun's bolts still fly (update with no trigger).
        weapon_.update(dt, false, muzzle, fwd);
        if (trigger && gunValid_ && rocketCooldown_ <= 0 && !rocketTriggerHeld_) {
            rockets_.push_back({muzzle, fwd * 32.0f, 0.0f});
            rocketCooldown_ = 1.0f;
            haptics_.push_back({gh, 0.9f, 0.12f});
            sfx("me_msls1", muzzle, 1.0f, 0.95f + 0.1f * frand());
        }
        rocketTriggerHeld_ = trigger;
        return;
    }
    rocketTriggerHeld_ = trigger;
    if (weapon_.update(dt, trigger && gunValid_, muzzle, fwd)) {
        haptics_.push_back({gh, 0.35f, 0.04f});
        sfx((weapon_.shotsFired() & 1) ? "me_peas1" : "me_peas2", muzzle, 0.7f, 0.95f + 0.1f * frand());
    }
}

// Wrist health bar (first person, left controller) and the red damage flash.
void GameApp::renderHud() {
    if (!impactModel_ || impactModel_->parts.empty() || !whiteTex_) return;
    const MeshId quad = impactModel_->parts[0].mesh;  // 0.2 m square in the XY plane
    auto drawQuad = [&](const Mat4& xf, const Vec4& color) {
        DrawParams dp;
        dp.texture = whiteTex_;
        dp.blend = BlendMode::AlphaBlend;
        dp.unlit = true;
        dp.noFog = true;
        dp.depthWrite = false;
        dp.doubleSided = true;
        dp.tint = color;
        renderer_.draw(quad, xf, dp);
    };
    const ControllerState& lc = lastInput_.hand[rig_.moveHand()];
    if (firstPerson_ && lc.gripPoseValid) {
        const Pose g = rig_.toWorld(lc.gripPose);
        const Mat4 base = Mat4::translation(g.position + Vec3{0, 0.09f, 0}) * Mat4::rotation(headWorldRot_);
        const float w = 0.13f, h = 0.016f, f = std::max(0.0f, playerHealth_) / 100.0f;
        drawQuad(base * Mat4::scale({(w + 0.008f) / 0.2f, (h + 0.008f) / 0.2f, 1}), {0, 0, 0, 0.6f});
        const Vec4 col{std::min(1.0f, 2.0f * (1.0f - f)), std::min(1.0f, 2.0f * f), 0.1f, 0.9f};
        drawQuad(base * Mat4::translation({-w * 0.5f * (1.0f - f), 0, 0.002f}) * Mat4::scale({w * f / 0.2f, h / 0.2f, 1}), col);
        // Jetpack fuel underneath (blue), once the jetpack is in the inventory.
        if (player_.jetpackAvailable) {
            const float jf = player_.fuel();
            const Mat4 fb = base * Mat4::translation({0, -0.024f, 0});
            drawQuad(fb * Mat4::scale({(w + 0.008f) / 0.2f, (h * 0.7f + 0.008f) / 0.2f, 1}), {0, 0, 0, 0.6f});
            drawQuad(fb * Mat4::translation({-w * 0.5f * (1.0f - jf), 0, 0.002f}) *
                         Mat4::scale({w * jf / 0.2f, h * 0.7f / 0.2f, 1}),
                     {0.2f, 0.6f, 1.0f, 0.9f});
        }
        // Mission objectives above the health bar: one pip each, gold when done.
        const size_t n = objectives_.size();
        for (size_t i = 0; i < n && n <= 16; ++i) {
            const float px = (float(i) - 0.5f * float(n - 1)) * 0.018f;
            const Mat4 pm = base * Mat4::translation({px, 0.022f, 0});
            drawQuad(pm * Mat4::scale({0.013f / 0.2f, 0.013f / 0.2f, 1}), {0, 0, 0, 0.6f});
            const Vec4 col = objectives_[i].done ? Vec4{1.0f, 0.8f, 0.1f, 0.95f} : Vec4{0.5f, 0.5f, 0.5f, 0.6f};
            drawQuad(pm * Mat4::translation({0, 0, 0.002f}) * Mat4::scale({0.009f / 0.2f, 0.009f / 0.2f, 1}), col);
        }
    }
    if (damageFlash_ > 0) {
        const Vec3 fwd = headWorldRot_.rotate(Vec3{0, 0, -1});
        const Vec3 eye = renderer_.currentView().eyePosition;
        drawQuad(Mat4::translation(eye + fwd * 0.12f) * Mat4::rotation(headWorldRot_) * Mat4::scale({4, 4, 1}),
                 {1.0f, 0.05f, 0.0f, std::min(0.5f, 0.35f * damageFlash_)});
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
    updateSun(dt);
    // Story progression: once a mission is complete and its outro has played, the screen fades to
    // the loading / briefing screen (as the original goes through its mission briefing), the next
    // mission loads behind it, and play continues on its own after a few seconds.
    if (missionComplete_ && playMode_ && !cutsceneActive() && assetWorld_ && load_.phase == LoadPhase::Idle && !advanceDone_) {
        advanceDone_ = true;
        const std::string next = assetWorld_->nextMission();
        if (!next.empty()) beginLoading(next, in);
    }
    // The first mission's briefing, before its opening cutscene (the level is already loaded).
    if (!firstBriefDone_ && std::getenv("GVR_NO_BRIEF")) firstBriefDone_ = true;  // test hook (headless default)
    if (!firstBriefDone_ && assetWorld_ && !assetWorld_->missionName().empty() && in.headValid && playMode_) {
        firstBriefDone_ = true;
        beginLoading(assetWorld_->missionName(), in, false);
    }
    if (load_.phase != LoadPhase::Idle) {
        updateLoading(dt, in);
        lastInput_ = in;
        return;
    }
    if (needSpawn_ && in.headValid) {
        rig_.teleportHead(in, assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
        if (playMode_) {
            teleportPlayer(assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
            if (!missionHostiles_ && objectives_.empty()) spawnWave();  // test waves only where the mission has no enemies or goals
            // The mission's opening cutscene (op 0x15), e.g. Baz's lander drop in 1a.
            const MissionData& m = assetWorld_->mission();
            // Test hook (headless): GVR_CUTSCENE=<first shot> plays that cutscene instead.
            const char* forced = std::getenv("GVR_CUTSCENE");
            if (forced && *forced) startCutscene(forced);
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
        if (missionEdge_.pressed(in.hand[mh].primary) && assetWorld_) {
            std::string next = assetWorld_->nextMission();
            if (next.empty() && !assetWorld_->missions().empty()) next = assetWorld_->missions().front();
            if (!next.empty()) {
                haptics_.push_back({mh, 0.7f, 0.1f});
                loadMission(next);
                setPlayMode(true, in);
            }
        }
        rig_.update(dt, in);
    }
    if (assetWorld_) assetWorld_->update(dt);

    // Haptic confirmation on trigger press (free camera only; in play mode the trigger fires).
    for (int h = 0; h < 2; ++h)
        if (triggerEdge_[h].pressed(in.hand[h].trigger > 0.6f) && !playMode_)
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
    if (assetWorld_ && assetWorld_->loaded()) renderSunRays();
    if (playMode_ && playerModel_ && !cut) playerModel_->render(renderer_, bodyXf_);
    if (playMode_ && gunValid_ && !cut) drawModel(weaponSel_ == 1 && launcherModel_ ? launcherModel_ : gunModel_, gunXf_, false);
    if (rocketModel_)
        for (const auto& r : rockets_) {
            // The rocket model points along -Y (game) = -Z (runtime): orient it along its flight.
            const Vec3 f = normalize(r.vel);
            const Quat yawQ = Quat::fromAxisAngle({0, 1, 0}, std::atan2(-f.x, -f.z));
            const Quat look = yawQ * Quat::fromAxisAngle({1, 0, 0}, std::asin(std::max(-1.0f, std::min(1.0f, f.y))));
            drawModel(rocketModel_, Mat4::translation(r.pos) * Mat4::rotation(look) * Mat4::scale({0.6f, 0.6f, 0.6f}), false);
        }
    for (const auto& c : chars_) {
        if (!c.visible || length(c.pos - v.eyePosition) > 160.0f) continue;
        if (cut && std::find(cutTypes.begin(), cutTypes.end(), c.type) != cutTypes.end()) continue;
        c.model->render(renderer_, Mat4::translation(c.pos + Vec3{0, c.lift, 0}) *
                                       Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, c.yaw)) *
                                       Mat4::scale({c.scale, c.scale, c.scale}));
    }
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
        m->pose(renderer_, std::min(f.age, m->animDuration() * 0.999f));
        m->render(renderer_, Mat4::translation(f.pos) * Mat4::scale(Vec3{1, 1, 1} * f.scale));
    }
    if (piranhaVis_ > 0.01f && pirSwim_) {
        // Circling just under the surface; one now and then leaps (pir_swim_jump) toward the player.
        pirSwim_->pose(renderer_, float(time_));
        for (int i = 0; i < kPiranhas; ++i) {
            const Piranha& f = piranha_[i];
            const float r = 1.2f + 0.35f * float(i % 3);
            const Vec3 at = piranhaCentre_ + Vec3{std::cos(f.angle) * r, 0, std::sin(f.angle) * r};
            const float yaw = std::atan2(-(-std::sin(f.angle)), -std::cos(f.angle));  // swimming along the circle
            const float depth = -0.25f - (1.0f - piranhaVis_) * 1.5f;
            if (f.jump >= 0 && pirJump_) {
                pirJump_->pose(renderer_, f.jump);
                const Vec3 to = piranhaCentre_ - at;
                pirJump_->render(renderer_, Mat4::translation(Vec3{at.x, depth, at.z}) *
                                                Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, std::atan2(-to.x, -to.z))));
                pirSwim_->pose(renderer_, float(time_));
            } else {
                pirSwim_->render(renderer_, Mat4::translation(Vec3{at.x, depth, at.z}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, yaw)));
            }
        }
    }
    for (const auto& g : gibs_)
        drawModel(g.model, Mat4::translation(g.pos) * Mat4::rotation(Quat::fromAxisAngle(g.axis, g.angle)) *
                               Mat4::scale({g.scale, g.scale, g.scale}),
                  false);
    for (const auto& e : enemies_.enemies)
        if (e.slot >= 0)
            enemyPool_[size_t(e.slot)]->render(renderer_, Mat4::translation(e.pos + Vec3{0, slotLift_[size_t(e.slot)], 0}) *
                                                              Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, e.yaw)));
    // Reaper bolts: the same spark, tinted red.
    for (const auto& p : enemyFire_.projectiles()) {
        const Quat r = rotationBetween({0, 0, -1}, p.vel);
        for (const auto& part : boltModel_ ? boltModel_->parts : std::vector<RenderModel::Part>{}) {
            DrawParams dp;
            dp.texture = part.texture;
            dp.blend = BlendMode::Additive;
            dp.unlit = true;
            dp.depthWrite = false;
            dp.doubleSided = true;
            dp.tint = {3.0f, 0.6f, 0.4f, 1.0f};
            renderer_.draw(part.mesh, Mat4::translation(p.pos) * Mat4::rotation(r) * Mat4::scale({0.6f, 0.6f, 0.6f}), dp);
        }
    }
    for (const auto& e : enemyFire_.effects())
        if (e.kind == WeaponEffect::Impact)
            drawModel(impactModel_, Mat4::translation(e.pos) * Mat4::rotation(headWorldRot_) *
                                        Mat4::scale(Vec3{1, 1, 1} * (2.0f + 3.0f * e.progress())), true, 1.0f - e.progress());
    // Bolts: the original spark quads (lying across the model's -Z axis) face along the flight,
    // drawn bright with a short fading trail so the shot reads as a blaster bolt.
    for (const auto& p : weapon_.projectiles()) {
        const Quat r = rotationBetween({0, 0, -1}, p.vel);
        const Vec3 back = normalize(p.vel) * -0.35f;
        for (int i = 0; i < 3; ++i) {
            const float s = 0.55f - 0.12f * float(i);
            if (p.travelled < 0.35f * float(i)) break;  // no trail behind the muzzle
            drawModel(boltModel_, Mat4::translation(p.pos + back * float(i)) * Mat4::rotation(r) * Mat4::scale({s, s, s}),
                      true, 2.5f - 0.7f * float(i));
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
        if (e.kind == WeaponEffect::Muzzle)
            drawModel(muzzleModel_, Mat4::translation(e.pos) * Mat4::rotation(rotationBetween({0, 0, -1}, e.dir)) *
                                        Mat4::scale(Vec3{1, 1, 1} * (0.25f + 0.15f * k)), true, 1.0f - k);
        else  // impact flash: billboard toward the viewer
            drawModel(impactModel_, Mat4::translation(e.pos) * Mat4::rotation(headWorldRot_) *
                                        Mat4::scale(Vec3{1, 1, 1} * (3.0f + 4.0f * k)), true, 1.0f - k);
    }
    if (cut) renderCutscene();
    else if (playMode_) renderHud();
    if (showTestScene_ || !hasOriginalAssets()) testScene_.render(renderer_, rig_, lastInput_);
    else if (!(playMode_ && firstPerson_ && playerModel_)) testScene_.renderControllers(renderer_, rig_, lastInput_);
    if (load_.phase == LoadPhase::FadeOut) renderLoading();
    renderer_.endView();
}

void GameApp::shutdown() {
    playerModel_.reset();
    assetWorld_.reset();
    renderer_.shutdown();
}

}  // namespace gvr
