// GameApp: Delphi, the Sea Reaper (the Reaper story's hero) - her model and clips, her sword and
// her fire bolt. Same class; see game_app.h ("Playable heroes").
#include "game/game_app.h"
#include "game/hero_clips.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "audio/mixer.h"
#include "core/log.h"
#include "game/arm_ik.h"
#include "game/cutscene.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr int kCreatureTargetD = 1000;  // as game_app.cpp's kCreatureTarget
constexpr float kMeccHeightD = 1.8f;    // the player capsule's default height
}

GameApp::Hero GameApp::heroForWorld(const std::string& world) {
    const std::string w = toLower(world);
    if (w.rfind("rstory", 0) == 0) return Hero::Delphi;
    if (w.rfind("kstory", 0) == 0) return Hero::Kabuto;
    return Hero::Mecc;
}

bool GameApp::loadDelphi() {
    if (delphiModel_) return true;
    if (!assetWorld_) return false;
    auto m = std::make_unique<SkinnedModel>();
    if (!m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "rp_l0", "rp_stand.anm")) return false;
    // Her clips for each PlayerAnim (hero_clips.h).
    const char* const* clips = kDelphiClips;
    for (int i = 1; i < int(PlayerAnim::Count); ++i) delphiClip_[i] = std::max(0, m->addAnimation(assetWorld_->fs(), clips[i]));
    delphiClip_[0] = 0;
    {
        const int die = m->addAnimation(assetWorld_->fs(), "rp_die.anm");
        const int fall = m->addAnimation(assetWorld_->fs(), "rp_land_dead.anm");
        heroDeath_[int(Hero::Delphi)][0] = die;
        heroDeath_[int(Hero::Delphi)][1] = die;
        heroDeath_[int(Hero::Delphi)][2] = fall >= 0 ? fall : die;
    }
    for (PlayerAnim a : {PlayerAnim::Land, PlayerAnim::LandHard, PlayerAnim::Inject, PlayerAnim::Attack1, PlayerAnim::Attack2,
                         PlayerAnim::Attack3, PlayerAnim::Cast})
        delphiOneShot_[int(a)] = m->animDuration(delphiClip_[int(a)]);
    // First person: her head hidden (the eyes sit inside it), her arms reaching for the
    // controllers (the same IK as the Meccaryn's; her chain is L_shoulder > L_arm01 > L_arm02 >
    // L_arm03, the hand). The eye point from the idle pose, as for him.
    m->setHiddenBones(SkinnedModel::headBones());
    m->setKeepFull(true);
    m->pose(renderer_, 0.0f);
    {
        Vec3 hmn, hmx;
        if (m->boneVertexBounds("head", hmn, hmx)) delphiEye_ = {0.5f * (hmn.x + hmx.x), hmn.y + 0.55f * (hmx.y - hmn.y), hmn.z - 0.03f};
        delphiEyeFromHead_ = delphiEye_ - m->hiddenCentroid();
        LOGI("Delphi eye point (model, m): %.2f %.2f %.2f", delphiEye_.x, delphiEye_.y, delphiEye_.z);
    }
    m->setPoseModifier([this](const AnmAnimation& clip, const std::vector<std::vector<int>>& children, std::vector<Mat4>& world) {
        if (!playMode_ || !firstPerson_ || hero_ != Hero::Delphi) return;
        auto node = [&](const char* n) {
            auto it = clip.byName.find(n);
            return it == clip.byName.end() ? -1 : it->second;
        };
        static const char* names[2][4] = {{"l_shoulder", "l_arm01", "l_arm02", "l_arm03"}, {"r_shoulder", "r_arm01", "r_arm02", "r_arm03"}};
        for (int h = 0; h < 2; ++h) {
            if (!armTargets_[h].valid) continue;
            ArmChain c;
            c.clavicle = node(names[h][0]);
            c.upper = node(names[h][1]);
            c.fore = node(names[h][2]);
            c.hand = node(names[h][3]);
            // Her fingers, index to little (by their distance from the thumb in the rest pose).
            static const char* fingers[2][4] = {{"l_fingers11", "l_fingers10", "l_fingers06", "l_fingers01"},
                                                {"r_finger13", "r_finger01", "r_finger06", "r_finger02"}};
            for (int f = 0; f < 4; ++f) c.finger[f] = node(fingers[h][f]);
            if (!c.valid()) continue;
            ArmTarget t;
            t.grip = armTargets_[h].grip;
            t.forward = armTargets_[h].forward;
            t.up = armTargets_[h].up;
            t.pole = Vec3{h == kLeft ? 0.7f : -0.7f, 0.4f, -1.0f} - normalize(t.up) * 0.5f;
            // Rework (user: "Delphi's hands in first person are tiny, the sword isn't in her hand"):
            // longer reach so the hand always arrives at the controller, and bigger hands.
            t.armStretch = 1.25f;
            t.maxReachStretch = 1.9f;
            t.handScale = 1.45f;
            const float res = solveArmIk(world, children, c, t);
            static const bool ikTrace = std::getenv("GVR_IK_TRACE") != nullptr;
            static int ikN = 0;
            if (ikTrace && (++ikN % 72) == 0) LOGI("ik: Delphi hand %d misses the controller by %.3f m", h, res);
        }
    });
    m->pose(renderer_, 0.0f);
    delphiModel_ = std::move(m);
    swordModel_ = assetWorld_->models()->get("rp_sword.gbs", "", /*dynamicLit=*/true);
    LOGI("Delphi loaded (rp_l0, %d clips), sword %s", int(PlayerAnim::Count), swordModel_ ? "ok" : "missing");
    return true;
}

// The body of a hero (movement, size, one-shot clip lengths) on a controller: the player's when the
// hero changes, a skirmish bot's. Kabuto's needs loadKabuto (his size comes from his model).
void GameApp::heroBody(Hero h, PlayerController& pc, float& scale, float& lift) const {
    PlayerParams p = meccParams_;
    pc.swimSpeed = 2.4f;
    pc.floatDepth = 1.35f;
    pc.radius = 0.35f;
    pc.height = kMeccHeightD;
    pc.stepUp = 0.45f;
    pc.jetpackAvailable = true;
    scale = 1.0f;
    lift = 0.0f;
    const float* shots = meccOneShot_;
    if (h == Hero::Delphi) {
        shots = delphiOneShot_;
        p.jumper = true;
        p.runSpeed = 5.5f;
        p.sprintFactor = 1.9f;    // turbo
        p.strideSpeed = 2.8f;
        p.jumpSpeed = 10.0f;
        p.stunSpeed = 22.0f;      // agile: lands bigger drops
        p.hardLandSpeed = 9.0f;
        pc.swimSpeed = 4.2f;      // a Sea Reaper swims fast
        pc.jetpackAvailable = false;
    } else if (h == Hero::Kabuto) {
        // Kabuto: about 15 Meccaryns tall. Walk and run speeds from his clips' foot travel
        // (kb_walk ~8.7 m/s, kb_run1 ~13 m/s); heavier gravity so his jumps don't float.
        shots = kabutoOneShot_;
        const float H = std::max(kMeccHeightD, kabutoEye_ / 0.9f);
        scale = H / kMeccHeightD;
        lift = kabutoFeet_;
        p.jumper = true;
        // Rework (user, device test 2026-10-25: "he is too slow"): a giant covers ground - 14 m/s
        // walking, 23 m/s charging; his clips play faster to keep up.
        p.runSpeed = 14.0f;
        p.strideSpeed = 8.7f;
        p.turboStride = 13.0f;
        p.sprintFactor = 1.65f;
        p.strafeStride = 6.0f;
        p.maxPlayback = 2.2f;
        p.turnRate = 5.0f;
        p.gravity = 30.0f;
        p.jumpSpeed = 24.0f;      // ~10 m up
        p.airControl = 10.0f;
        p.stunSpeed = 1e9f;       // a giant takes no fall damage
        p.hardLandSpeed = 26.0f;
        p.stepDown = 0.6f * scale;
        p.maxStepUp = 0.6f * scale;
        p.slideMaxSpeed = 18.0f;
        // Rework: a giant scrambles up what stops a man - banks and cliff faces up to 68 degrees (the
        // Meccaryn's limit is 50). kstory3_4's portal stands on an island ringed by cliffs that 50
        // degrees never climbs (harness reach check).
        p.maxSlope = 2.5f;
        pc.radius = 0.11f * H;
        pc.height = H;
        pc.stepUp = 0.3f * scale;
        pc.floatDepth = 0.7f * H;  // he wades; deeper water than that and he swims (badly)
        pc.swimSpeed = 4.0f;
        pc.jetpackAvailable = false;
    }
    std::copy(shots, shots + int(PlayerAnim::Count), std::begin(pc.oneShot));
    if (h != Hero::Mecc) {
        if (shots[int(PlayerAnim::LandHard)] > 0.3f) p.stunSeconds = shots[int(PlayerAnim::LandHard)];
        if (shots[int(PlayerAnim::Land)] > 0.2f) p.landSeconds = shots[int(PlayerAnim::Land)];
    }
    pc.params = p;
}

void GameApp::setHero(Hero h) {
    resetSpells();
    if (!meccParamsSaved_) {
        meccParams_ = player_.params;
        std::copy(std::begin(clip_), std::end(clip_), std::begin(meccClip_));
        std::copy(std::begin(player_.oneShot), std::end(player_.oneShot), std::begin(meccOneShot_));
        std::copy(std::begin(deathClip_), std::end(deathClip_), std::begin(heroDeath_[int(Hero::Mecc)]));
        meccParamsSaved_ = true;
    }
    if (h == hero_) return;
    if (h == Hero::Delphi && !loadDelphi()) { LOGW("Delphi unavailable: hero unchanged"); return; }
    if (h == Hero::Kabuto && !loadKabuto()) { LOGW("Kabuto unavailable: hero unchanged"); return; }
    // Park the current hero's model.
    switch (hero_) {
        case Hero::Mecc: meccMain_ = std::move(playerModel_); meccSpare_ = std::move(playerSpare_); break;
        case Hero::Delphi: delphiModel_ = std::move(playerModel_); break;
        case Hero::Kabuto: kabutoModel_ = std::move(playerModel_); break;
    }
    swordValid_ = false;
    gunValid_ = false;
    tpAiming_ = false;
    const int* clips = meccClip_;
    if (!meccEyeSaved_) { meccEye_ = eyeModel_; meccEyeFromHead_ = eyeFromHead_; meccEyeSaved_ = true; }
    // The first-person eye point of the body worn.
    eyeModel_ = h == Hero::Delphi ? delphiEye_ : (h == Hero::Kabuto ? kabutoEyeModel_ : meccEye_);
    eyeFromHead_ = h == Hero::Delphi ? delphiEyeFromHead_ : (h == Hero::Kabuto ? kabutoEyeFromHead_ : meccEyeFromHead_);
    eyeNow_ = eyeModel_;
    if (h == Hero::Mecc) {
        playerModel_ = std::move(meccMain_);
        playerSpare_ = std::move(meccSpare_);
    } else if (h == Hero::Delphi) {
        playerModel_ = std::move(delphiModel_);
        clips = delphiClip_;
    } else {
        playerModel_ = std::move(kabutoModel_);
        clips = kabutoClip_;
    }
    std::copy(clips, clips + int(PlayerAnim::Count), std::begin(clip_));
    {
        const bool jet = player_.jetpackAvailable;  // the Meccaryn's is the mission's to give
        heroBody(h, player_, heroScale_, heroLift_);
        if (h == Hero::Mecc) player_.jetpackAvailable = jet;
    }
    const PlayerParams p = player_.params;
    std::copy(std::begin(heroDeath_[int(h)]), std::end(heroDeath_[int(h)]), std::begin(deathClip_));
    hero_ = h;
    if (h == Hero::Kabuto) {
        kbBaseParams_ = p;
        kbBaseScale_ = heroScale_;
        kbBaseH_ = player_.height;
        kbGrow_ = kbGrowShown_ = 1.0f;
    }
    kb_ = KabutoState{};
    LOGI("Hero: %s (scale %.1f)", h == Hero::Delphi ? "Delphi (Sea Reaper)" : (h == Hero::Kabuto ? "Kabuto" : "the Meccaryn"), heroScale_);
}

void GameApp::updateDelphi(float dt, const InputFrame& in) {
    gunValid_ = false;
    swordValid_ = false;
    if (!playerModel_) return;
    // "You can regenerate by immersing yourself in deep water if you are badly wounded" (the Reaper
    // story's mission hints): health returns while she swims (rate ours).
    if (player_.swimming() && playerHealth_ > 0 && playerHealth_ < 100.0f) {
        playerHealth_ = std::min(100.0f, playerHealth_ + 6.0f * dt);
        healFlash_ = std::max(healFlash_, 0.15f);
    }
    weaponsTicked_ = true;
    scopeWanted_ = false;  // (a Meccaryn mission's sniper)
    if (mgLoop_ && audio_) { audio_->stop(mgLoop_); mgLoop_ = 0; }
    updateBow(dt, in);  // her bows (game_app_bow.cpp); with one in hand the sword is put away
    const int gh = rig_.turnHand(), mh = rig_.moveHand();
    for (auto& c : swordCool_) c.second -= dt;
    swordCool_.erase(std::remove_if(swordCool_.begin(), swordCool_.end(), [](const std::pair<int, float>& c) { return c.second <= 0; }),
                     swordCool_.end());
    auto cooled = [&](int target) {
        for (const auto& c : swordCool_)
            if (c.first == target) return false;
        return true;
    };
    // --- The sword: in the weapon hand (first person: the controller, blade along where it
    // points; third person: Ref_Rhand as the original attaches it).
    if (swordModel_ && delphiWeapon_ < 0) {
        if (firstPerson_) {
            const ControllerState& c = in.hand[gh];
            if (c.gripPoseValid) {
                const Pose grip = rig_.toWorld(c.gripPose);
                const Pose aim = c.aimPoseValid ? rig_.toWorld(c.aimPose) : grip;
                const Quat q = aim.orientation * rotationBetween(Vec3{0, 1, 0}, Vec3{0, 0, -1});
                swordXf_ = Mat4::translation(grip.position) * Mat4::rotation(q);
                swordValid_ = true;
            }
        } else {
            Mat4 ref;
            if (playerModel_->nodeWorld("ref_rhand", ref)) {
                swordXf_ = bodyXf_ * gameMatrixToRuntime(ref);
                swordValid_ = true;
            }
        }
    }
    const float bladeLen = swordModel_ ? swordModel_->boundsMax.y * 0.98f : 0.75f;
    auto bladePoint = [&](float y) {
        const Vec4 p = swordXf_ * Vec4{0, y, 0, 1};
        return Vec3{p.x, p.y, p.z};
    };
    auto strike = [&](int target, const Vec3& at, const Vec3& dir, float mul) {
        hitMul_ = mul;
        weapon_.onHit(target, at, dir);
        hitMul_ = 1.0f;
        swordCool_.push_back({target, 0.4f});
        static const char* hits[] = {"sr_swht1", "sr_swht2", "sr_swht3", "sr_swht4"};
        sfx(hits[int(frand() * 3.99f)], at, 1.0f);
        haptics_.push_back({gh, 0.9f, 0.08f});
    };
    // First person: swing it. A fast-moving blade cuts what it passes through (sweep from the
    // tip's last position and along the blade); harder swings hit harder.
    if (firstPerson_ && swordValid_ && weapon_.raycast && weapon_.onHit) {
        const Vec3 hilt = bladePoint(0.12f), tip = bladePoint(bladeLen);
        // Rework: Reaper bolts meeting the blade are deflected (game_app_parry.cpp).
        parryShots(hilt, tip, swordHiltPrev_, swordTipPrev_, swordTipValid_, swordTipValid_ && dt > 0 ? (tip - swordTipPrev_) * (1.0f / dt) : Vec3{0, 0, 0}, gh, dt);
        updateSwordTrail(hilt, tip, dt);
        if (swordTipValid_ && dt > 0) {
            const float speed = length(tip - swordTipPrev_) / dt;
            if (speed > 3.0f) {
                if (speed > 6.0f && frand() < dt * 6.0f) sfx(frand() < 0.5f ? "sr_swsh1" : "sr_swsh2", tip, 0.6f);
                const Vec3 segs[2][2] = {{swordTipPrev_, tip}, {hilt, tip}};
                for (const auto& sg : segs) {
                    Vec3 d = sg[1] - sg[0];
                    const float l = length(d);
                    if (l < 1e-3f) continue;
                    d = d * (1.0f / l);
                    float t;
                    Vec3 n;
                    int target = -1;
                    if (weapon_.raycast(sg[0], d, l, t, n, target) && target >= 0 && cooled(target)) {
                        const Vec3 at = sg[0] + d * t;
                        const Vec3 mv = normalize(tip - swordTipPrev_);
                        const Vec3 axis = normalize(tip - hilt);
                        // Rework: a thrust (the blade driven along its own line) runs him through, a
                        // hard slash takes him apart; the rest are cuts as before.
                        const bool thrust = dot(mv, axis) > 0.7f && impaledUid_ == 0;
                        const bool slash = !thrust && speed > 8.0f;
                        const bool reaper = target < kCreatureTargetD;
                        const uint32_t uid = reaper && size_t(target) < enemies_.enemies.size() ? enemies_.enemies[size_t(target)].uid : 0;
                        endCloak("she struck");
                        strike(target, at, mv, std::min(2.5f, 0.6f + speed / 5.0f) * (thrust ? 4.0f : 1.0f));  // (a thrust is deadly)
                        if (reaper && uid) {
                            const int ei = enemyOfUid(uid);
                            if (ei >= 0 && enemies_.enemies[size_t(ei)].state == EnemyState::Dead) {
                                Enemy& e = enemies_.enemies[size_t(ei)];
                                if (thrust) {
                                    impaledUid_ = uid;
                                    impaleAlong_ = std::max(0.25f * bladeLen, std::min(0.75f * bladeLen, dot(at - hilt, axis)));
                                    impaleT_ = 0;
                                    // Hanging on the blade: a ragdoll pinned there (no model slot: a limp, swinging clip).
                                    if (!startRagdoll(e, mv * 0.3f, Vec3{0, 0, 0})) enemies_.setBodyClip(ei, EnemyClip::FallCycle, true);
                                    sfx("ex_bdys1", at, 0.9f, 0.8f);
                                    haptics_.push_back({gh, 1.0f, 0.2f});
                                    LOGI("Delphi runs Reaper %u through", uid);
                                } else if (slash && !e.gibbed) {
                                    // The limb the blade crossed comes off (head, arm, leg); the body falls
                                    // as a ragdoll. (No limb near the cut: the old burst of pieces.)
                                    if (dismember(e, at, mv)) {
                                        bleed(at, mv, true, 1.2f);
                                        LOGI("Delphi's slash cuts a limb off Reaper %u", uid);
                                    } else {
                                        e.gibbed = true;
                                        gibCorpse(e.pos, e.yaw, mv, true, 1.0f);
                                        bleed(at, mv, true, 1.5f);
                                        blood_.pool(e.pos, 1.2f);
                                        LOGI("Delphi's slash cuts Reaper %u apart", uid);
                                    }
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
        // Rework: a Reaper run through hangs on the blade until it is yanked out (pulled back along
        // the blade, or a hard flick) - then he slides off and drops.
        if (impaledUid_) {
            const int ei = enemyOfUid(impaledUid_);
            impaleT_ += dt;
            const Vec3 axis = normalize(tip - hilt);
            const Vec3 tipVel = swordTipValid_ && dt > 0 ? (tip - swordTipPrev_) * (1.0f / dt) : Vec3{0, 0, 0};
            const bool yank = dot(tipVel, axis) < -2.5f || length(tipVel) > 9.0f || impaleT_ > 8.0f;
            if (ei < 0 || enemies_.enemies[size_t(ei)].gibbed) {
                impaledUid_ = 0;
            } else if (yank) {
                Enemy& e = enemies_.enemies[size_t(ei)];
                if (Ragdoll* rd = ragdollOf(e)) {  // off the blade with its motion
                    rd->unpin();
                    rd->kick(tipVel * 0.5f);
                } else {
                    e.vel = tipVel * 0.5f;
                    enemies_.setBodyClip(ei, EnemyClip::FallLand, false);  // off the blade: he lies
                }
                impaledUid_ = 0;
                sfx("ex_bdys1", e.pos + Vec3{0, 1, 0}, 0.7f, 1.2f);
                LOGI("Delphi pulls the sword out of Reaper %u", e.uid);
            } else {
                Enemy& e = enemies_.enemies[size_t(ei)];
                const Vec3 p = hilt + axis * impaleAlong_;
                if (Ragdoll* rd = ragdollOf(e)) {
                    if (rd->active) rd->pin(p);
                    rd->posedAsleep = false;
                } else {
                    e.pos = p - Vec3{0, 0.55f, 0};  // (the flailing pose's middle on the blade)
                }
                e.vel = Vec3{0, 0, 0};
                if (frand() < dt * 3.0f) bleed(p, Vec3{0, -1, 0}, false, 0.7f);  // dripping
                if (frand() < dt * 4.0f) haptics_.push_back({gh, 0.3f, 0.04f});  // his weight
            }
        }
        swordTipPrev_ = tip;
        swordHiltPrev_ = hilt;
        swordTipValid_ = true;
    } else {
        swordTipValid_ = false;
        impaledUid_ = 0;
    }
    // Third person: the trigger swings - a three-swipe combo, the hit landing a third of the way in.
    const bool trig = in.hand[gh].trigger > 0.6f;
    comboTimer_ = std::max(0.0f, comboTimer_ - dt);
    if (!firstPerson_ && trig && !swipeTrigWas_ && swipeT_ < 0 && player_.onGround && delphiWeapon_ < 0) {
        comboStep_ = comboTimer_ > 0 ? (comboStep_ + 1) % 3 : 0;
        const PlayerAnim a = PlayerAnim(int(PlayerAnim::Attack1) + comboStep_);
        const float dur = std::max(0.3f, player_.oneShot[int(a)]);
        player_.startAction(a, dur);
        swipeT_ = 0;
        swipeHit_ = false;
        comboTimer_ = dur + 0.5f;
        sfx2D(frand() < 0.5f ? "sr_swsh1" : "sr_swsh2", 0.8f);
    }
    swipeTrigWas_ = trig;
    if (swipeT_ >= 0) {
        swipeT_ += dt;
        const PlayerAnim a = PlayerAnim(int(PlayerAnim::Attack1) + comboStep_);
        const float dur = std::max(0.3f, player_.oneShot[int(a)]);
        if (!swipeHit_ && swipeT_ > dur * 0.33f && weapon_.onHit) {
            swipeHit_ = true;
            const Vec3 fwd = Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(Vec3{0, 0, -1});
            auto inArc = [&](const Vec3& p, float reach) {
                Vec3 d = p - player_.position;
                d.y = 0;
                const float l = length(d);
                return l < reach && (l < 0.6f || dot(d * (1.0f / l), fwd) > 0.35f);
            };
            const float mul = comboStep_ == 2 ? 3.0f : 2.0f;  // the third swipe hits hardest
            for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
                const Enemy& e = enemies_.enemies[i];
                if (e.state != EnemyState::Dead && inArc(e.pos, 2.8f)) strike(int(i), e.pos + Vec3{0, 1.2f, 0}, fwd, mul);
            }
            for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
                const Creature& c = creatures_.creatures[i];
                if (c.state != CreatureState::Dead && inArc(c.pos, 2.4f + c.scale)) strike(kCreatureTargetD + int(i), c.pos + Vec3{0, 0.6f * c.scale, 0}, fwd, mul);
            }
            for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
                const Vimp& v = vimps_.vimps[i];
                if (v.state != VimpState::Dead && inArc(v.pos, 3.6f)) strike(kVimpTarget + int(i), v.pos + Vec3{0, 2.0f, 0}, fwd, mul);
            }
            if (skirmish_)
                for (const auto& p : sk_.players)
                    if (p.alive && !p.local && sk_.enemies(p.id, skLocal_) && inArc(p.body.position, 2.6f + p.body.radius))
                        strike(kBotTarget + p.id, p.body.position + Vec3{0, 0.6f * std::min(p.body.height, 2.0f), 0}, fwd, mul);
        }
        if (swipeT_ > dur || !player_.inAction()) swipeT_ = -1;
    }
    // Fire bolt: the move hand's trigger (first person: from that hand where it points; third
    // person: from her left hand toward the crosshair or straight ahead).
    boltCooldown_ -= dt;
    const bool boltTrig = in.hand[mh].trigger > 0.6f && !(firstPerson_ && delphiBow());  // (first person the bow is in that hand)
    // Where her casting hand aims (first person: that hand where it points; third person: from her
    // left hand toward the crosshair or straight ahead).
    Vec3 from, dir;
    bool ok = false;
    if (firstPerson_) {
        const ControllerState& c = in.hand[mh];
        if (c.aimPoseValid) {
            const Pose aim = rig_.toWorld(c.aimPose);
            dir = aim.orientation.rotate(Vec3{0, 0, -1});
            from = aim.position + dir * 0.25f;
            ok = true;
        }
    } else {
        Mat4 ref;
        if (playerModel_->nodeWorld("ref_lhand", ref)) {
            const Vec4 p = bodyXf_ * Vec4{-ref.m[12] * gGameUnitsToMeters, ref.m[14] * gGameUnitsToMeters, ref.m[13] * gGameUnitsToMeters, 1};
            from = {p.x, p.y, p.z};
            dir = tpAiming_ ? normalize(aimTarget_ - from) : Quat::fromAxisAngle({0, 1, 0}, player_.yaw).rotate(normalize(Vec3{0, 0.05f, -1}));
            ok = true;
        }
    }
    // Rework: her casting hand's own speed (tracking space, smoothed) - a bolt let go in a throwing
    // motion flies along the throw, faster.
    {
        const ControllerState& c = in.hand[mh];
        if (firstPerson_ && c.gripPoseValid && dt > 0) {
            const Vec3 v = castHandValid_ ? (c.gripPose.position - castHandPrev_) * (1.0f / dt) : Vec3{0, 0, 0};
            castHandVel_ = castHandVel_ * 0.4f + v * 0.6f;
            castHandPrev_ = c.gripPose.position;
            castHandValid_ = true;
        } else {
            castHandValid_ = false;
            castHandVel_ = Vec3{0, 0, 0};
        }
    }
    // A held trigger charges a spell (game_app_spells.cpp); a tap released is the fire bolt.
    updateSpellGestures(dt, in, dir);
    updateFollow(dt, in, dir);  // (FOLLOW: catching, holding, letting go)
    const bool consumed = updateSpells(dt, boltTrig, from, dir, ok);
    float boltSpeed = 28.0f;
    const Vec3 aimDir = dir;  // (the cluster flies along the aim, not the upward flick that casts it)
    if (firstPerson_ && length(castHandVel_) > 1.8f) {
        const Vec3 throwDir = normalize(rig_.worldFromTracking.orientation.rotate(castHandVel_));
        if (dot(throwDir, dir) > 0.3f) {
            dir = normalize(throwDir * 0.6f + dir * 0.4f);
            boltSpeed = 40.0f;
        }
    }
    // (2026-10-28) CLUSTER: the bolt let go while the hand flicks up - six bolts in a spread along the aim.
    if (!consumed && !boltTrig && boltTrigWas_ && ok && firstPerson_ && castHandVel_.y > 2.2f && clusterOwned() && clusterCool_ <= 0 &&
        boltCooldown_ <= 0 && !heldReaperGun(mh)) {
        const Vec3 dir = aimDir;
        const Vec3 side = normalize(cross(dir, Vec3{0, 1, 0}) + Vec3{1e-4f, 0, 0});
        for (int k = 0; k < 6; ++k) {
            const float a = (float(k) - 2.5f) * 0.09f;
            Rocket r;
            r.pos = from;
            r.vel = normalize(dir + side * a + Vec3{0, 0.03f * float(k % 2), 0}) * 32.0f;
            r.life = 0;
            r.kind = 1;
            rockets_.push_back(r);
        }
        clusterCool_ = 4.0f;
        boltCooldown_ = 1.0f;
        endCloak("she shot");
        sfx("sr_spfsh", from, 1.0f, 0.8f);
        haptics_.push_back({mh, 1.0f, 0.15f});
        LOGI("Delphi casts CLUSTER (hand up %.1f m/s)", castHandVel_.y);
        boltTrigWas_ = boltTrig;
        return;
    }
    if (!consumed && !boltTrig && boltTrigWas_ && boltCooldown_ <= 0 && ok && !heldReaperGun(mh)) {  // (that hand's trigger fires a held Reaper gun)
        endCloak("she shot");
        if (!firstPerson_ && player_.onGround && swipeT_ < 0) player_.startAction(PlayerAnim::Cast, std::max(0.3f, player_.oneShot[int(PlayerAnim::Cast)] * 0.6f));
        Rocket r;
        r.pos = from;
        r.vel = dir * boltSpeed;
        r.life = 0;
        r.kind = 1;
        if (boltSpeed > 30.0f) {
            haptics_.push_back({mh, 0.8f, 0.08f});
            static const bool trace = std::getenv("GVR_PARRY_TRACE") != nullptr;
            if (trace) LOGI("Delphi throws a fire bolt (hand %.1f m/s)", length(castHandVel_));
        }
        rockets_.push_back(r);
        boltCooldown_ = 1.0f;
        sfx("sr_spfsh", from, 1.0f);
        haptics_.push_back({mh, 0.6f, 0.08f});
    }
    boltTrigWas_ = boltTrig;
}

}  // namespace gvr
