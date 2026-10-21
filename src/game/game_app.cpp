#include "game/game_app.h"

#include "core/log.h"
#include "scene/asset_world.h"
#include "audio/mixer.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
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
            const char* clips[] = {"mc_stand.anm", "mc_run.anm", "mc_thrust.anm", "mc_hover.anm", "mc_land_soft.anm", "mc_sidestep.anm",
                                   "mc_skid.anm"};
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
            playerModel_ = std::move(m);
            playMode_ = true;
            AssetWorld* w = assetWorld_.get();
            // Weapon models: gun (held, so dynamically lit), bolt, muzzle flash, impact flash.
            ModelCache* mc = assetWorld_->models();
            gunModel_ = mc->get("mc_gun_l0.gbs", "", /*dynamicLit=*/true);
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
                const bool he = enemies_.raycast(o, d, maxD, te, ne, ie);
                if (!hw && !he) return false;
                if (he && (!hw || te < tw)) { t = te; n = ne; target = ie; }
                else { t = tw; n = nw; target = -1; }
                return true;
            };
            weapon_.onHit = [this](int target, const Vec3& p, const Vec3&) {
                if (target < 0) {
                    sfx(frand() < 0.5f ? "bi_dirt1" : (frand() < 0.5f ? "bi_hit1" : "bi_hit2"), p, 0.45f, 0.9f + 0.2f * frand());
                    return;
                }
                const bool killed = enemies_.damage(target, 25.0f);
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
            enemies_.groundHeight = [w](float x, float z) {
                float y;
                return w->terrain().heightAt(x, z, y) ? y : -1.2f;
            };
            enemies_.collision = &w->collision();
            // Reaper grunt model pool with the original clips (order = EnemyClip).
            const char* eclips[] = {"rpman_stand_scan.anm", "rpman_walk_fire.anm", "rpman_stand_fire.anm",
                                    "rpman_hit_med.anm", "rpman_hit_med2.anm", "rpman_hit_med_die.anm",
                                    "rpman_hit_spin_die.anm", "rpman_hit_gut_die.anm", "rpman_bzka_run.anm"};
            for (int k = 0; k < 6; ++k) {
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
            player_.collision = &w->collision();
            // Off the terrain heightfield is open sea (surface at 0): wading depth, so walking off
            // a cliff drops you into the water instead of walking on air.
            player_.groundHeight = [w](float x, float z) {
                float y;
                return w->terrain().heightAt(x, z, y) ? y : -1.2f;
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
    updateCombat(dt);
    updateAudio(dt);
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
        "me_jpkno", "me_fsgr1", "me_fsgr2", "me_hdfl1", "me_hdfl2", "me_sld1", "me_slden"};
    size_t ok = 0;
    for (const char* n : names) {
        std::vector<uint8_t> bytes;
        WavData w;
        std::string err;
        if (assetWorld_->fs().read(std::string(n) + ".wav", bytes) && parseWav(bytes.data(), bytes.size(), w, &err) &&
            audio_->addSound(n, std::move(w)))
            ++ok;
        else
            LOGW("Sound %s unavailable %s", n, err.c_str());
    }
    LOGI("Audio: %zu/%zu original sounds loaded", ok, sizeof(names) / sizeof(names[0]));
}

void GameApp::updateAudio(float dt) {
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

void GameApp::updateCombat(float dt) {
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
    if (enemies_.aliveCount() == 0) {
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
    Mat4 ref;
    Vec3 muzzle, fwd;
    if (firstPerson_) {
        // First person: the gun follows the controller's aim pose exactly (shots go where you
        // point); handle at the grip, sized with the body's hands.
        const ControllerState& c = in.hand[gh];
        gunValid_ = gunModel_ && c.aimPoseValid && c.gripPoseValid;
        if (gunValid_) {
            const Pose aim = rig_.toWorld(c.aimPose);
            const Pose grip = rig_.toWorld(c.gripPose);
            const float bodyScale = eyeModel_.y > 0.1f ? userEye_ / eyeModel_.y : 1.0f;
            const float k = bodyScale * 0.6f;
            const Vec3 handle = giantsToRuntime(kGunHandleGame.x, kGunHandleGame.y, kGunHandleGame.z);
            gunXf_ = Mat4::translation(grip.position) * Mat4::rotation(aim.orientation) * Mat4::scale({k, k, k}) *
                     Mat4::translation(handle * -1.0f);
        }
    } else {
        // Third person: gun in the hand (skeleton's Ref_Rhand position), pointing at the
        // crosshair while aiming, otherwise forward and slightly down; character-sized.
        gunValid_ = gunModel_ && playerModel_ && playerModel_->nodeWorld(gh == kRight ? "ref_rhand" : "ref_lhand", ref);
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
            const Vec3 handle = giantsToRuntime(kGunHandleGame.x, kGunHandleGame.y, kGunHandleGame.z);
            gunXf_ = Mat4::translation(hand) * Mat4::rotation(look) * Mat4::scale({k, k, k}) * Mat4::translation(handle * -1.0f);
        }
    }
    if (gunValid_) {
        const Vec3 mr = giantsToRuntime(kGunMuzzleGame.x, kGunMuzzleGame.y, kGunMuzzleGame.z);
        const Vec4 m4 = gunXf_ * Vec4{mr.x, mr.y, mr.z, 1.0f};
        const Vec3 fr = giantsToRuntime(kGunForwardGame.x, kGunForwardGame.y, kGunForwardGame.z);
        const Vec4 f4 = gunXf_ * Vec4{fr.x, fr.y, fr.z, 0.0f};
        muzzle = {m4.x, m4.y, m4.z};
        fwd = normalize(Vec3{f4.x, f4.y, f4.z});
        if (!firstPerson_ && tpAiming_) fwd = normalize(aimTarget_ - muzzle);  // shots go to the crosshair
    }
    const bool trigger = in.hand[gh].trigger > 0.6f;
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
        // Jetpack fuel underneath (blue).
        const float jf = player_.fuel();
        const Mat4 fb = base * Mat4::translation({0, -0.024f, 0});
        drawQuad(fb * Mat4::scale({(w + 0.008f) / 0.2f, (h * 0.7f + 0.008f) / 0.2f, 1}), {0, 0, 0, 0.6f});
        drawQuad(fb * Mat4::translation({-w * 0.5f * (1.0f - jf), 0, 0.002f}) * Mat4::scale({w * jf / 0.2f, h * 0.7f / 0.2f, 1}),
                 {0.2f, 0.6f, 1.0f, 0.9f});
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
    if (needSpawn_ && in.headValid) {
        rig_.teleportHead(in, assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
        if (playMode_) {
            teleportPlayer(assetWorld_->spawnPoint(), assetWorld_->spawnYaw());
            spawnWave();
        }
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
    if (assetWorld_ && assetWorld_->loaded()) assetWorld_->render(renderer_);
    if (playMode_ && playerModel_) playerModel_->render(renderer_, bodyXf_);
    if (playMode_ && gunValid_) drawModel(gunModel_, gunXf_, false);
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
    if (playMode_) renderHud();
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
