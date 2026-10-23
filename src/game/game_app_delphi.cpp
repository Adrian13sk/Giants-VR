// GameApp: Delphi, the Sea Reaper (the Reaper story's hero) - her model and clips, her sword and
// her fire bolt. Same class; see game_app.h ("Playable heroes").
#include "game/game_app.h"

#include <algorithm>
#include <cmath>

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
    // Her clips for each PlayerAnim (no jetpack: jumps, falls, swims, turbo, sword swipes, spell).
    static const char* clips[int(PlayerAnim::Count)] = {
        "rp_stand.anm",     "rp_run.anm",        "rp_jump_cycle.anm", "rp_swim_treadwater.anm", "rp_land.anm",
        "rp_side_r.anm",    "rp_slide.anm",      "rp_runjump_cycle.anm", "rp_jump_cycle.anm",   "rp_jump_cycle.anm",
        "rp_jump_cycle.anm", "rp_land_hard.anm", "rp_fall.anm",       "rp_spell.anm",           "rp_swim_breast.anm",
        "rp_turbo.anm",     "rp_swipe_1.anm",    "rp_swipe_2.anm",    "rp_swipe_3.anm",         "rp_spell_arm_bolt.anm"};
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
    m->pose(renderer_, 0.0f);
    delphiModel_ = std::move(m);
    swordModel_ = assetWorld_->models()->get("rp_sword.gbs", "", /*dynamicLit=*/true);
    LOGI("Delphi loaded (rp_l0, %d clips), sword %s", int(PlayerAnim::Count), swordModel_ ? "ok" : "missing");
    return true;
}

void GameApp::setHero(Hero h) {
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
    // Defaults (the Meccaryn's), then the new hero's differences.
    PlayerParams p = meccParams_;
    player_.swimSpeed = 2.4f;
    player_.floatDepth = 1.35f;
    player_.radius = 0.35f;
    player_.height = kMeccHeightD;
    player_.stepUp = 0.45f;
    heroScale_ = 1.0f;
    heroLift_ = 0.0f;
    swordValid_ = false;
    gunValid_ = false;
    tpAiming_ = false;
    const int* clips = meccClip_;
    const float* shots = meccOneShot_;
    if (h == Hero::Mecc) {
        playerModel_ = std::move(meccMain_);
        playerSpare_ = std::move(meccSpare_);
    } else if (h == Hero::Delphi) {
        playerModel_ = std::move(delphiModel_);
        clips = delphiClip_;
        shots = delphiOneShot_;
        p.jumper = true;
        p.runSpeed = 5.5f;
        p.sprintFactor = 1.9f;    // turbo
        p.strideSpeed = 2.8f;
        p.jumpSpeed = 10.0f;
        p.stunSpeed = 22.0f;      // agile: lands bigger drops
        p.hardLandSpeed = 9.0f;
        player_.swimSpeed = 4.2f;  // a Sea Reaper swims fast
        player_.jetpackAvailable = false;
    } else {
        // Kabuto: about 15 Meccaryns tall. Walk and run speeds from his clips' foot travel
        // (kb_walk ~8.7 m/s, kb_run1 ~13 m/s); heavier gravity so his jumps don't float.
        playerModel_ = std::move(kabutoModel_);
        clips = kabutoClip_;
        shots = kabutoOneShot_;
        const float H = std::max(kMeccHeightD, kabutoEye_ / 0.9f);
        heroScale_ = H / kMeccHeightD;
        heroLift_ = kabutoFeet_;
        p.jumper = true;
        p.runSpeed = 8.7f;
        p.strideSpeed = 8.7f;
        p.turboStride = 13.0f;
        p.sprintFactor = 1.55f;
        p.strafeStride = 6.0f;
        p.maxPlayback = 1.8f;
        p.turnRate = 3.0f;
        p.gravity = 30.0f;
        p.jumpSpeed = 24.0f;      // ~10 m up
        p.airControl = 10.0f;
        p.stunSpeed = 1e9f;       // a giant takes no fall damage
        p.hardLandSpeed = 26.0f;
        p.stepDown = 0.6f * heroScale_;
        p.maxStepUp = 0.6f * heroScale_;
        p.slideMaxSpeed = 18.0f;
        player_.radius = 0.11f * H;
        player_.height = H;
        player_.stepUp = 0.3f * heroScale_;
        player_.floatDepth = 0.7f * H;  // he wades; deeper water than that and he swims (badly)
        player_.swimSpeed = 4.0f;
        player_.jetpackAvailable = false;
    }
    std::copy(clips, clips + int(PlayerAnim::Count), std::begin(clip_));
    std::copy(shots, shots + int(PlayerAnim::Count), std::begin(player_.oneShot));
    if (h != Hero::Mecc) {
        if (shots[int(PlayerAnim::LandHard)] > 0.3f) p.stunSeconds = shots[int(PlayerAnim::LandHard)];
        if (shots[int(PlayerAnim::Land)] > 0.2f) p.landSeconds = shots[int(PlayerAnim::Land)];
    }
    player_.params = p;
    std::copy(std::begin(heroDeath_[int(h)]), std::end(heroDeath_[int(h)]), std::begin(deathClip_));
    hero_ = h;
    kb_ = KabutoState{};
    LOGI("Hero: %s (scale %.1f)", h == Hero::Delphi ? "Delphi (Sea Reaper)" : (h == Hero::Kabuto ? "Kabuto" : "the Meccaryn"), heroScale_);
}

void GameApp::updateDelphi(float dt, const InputFrame& in) {
    gunValid_ = false;
    swordValid_ = false;
    if (!playerModel_) return;
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
    if (swordModel_) {
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
                        strike(target, sg[0] + d * t, normalize(tip - swordTipPrev_), std::min(2.5f, 0.6f + speed / 5.0f));
                        break;
                    }
                }
            }
        }
        swordTipPrev_ = tip;
        swordTipValid_ = true;
    } else {
        swordTipValid_ = false;
    }
    // Third person: the trigger swings - a three-swipe combo, the hit landing a third of the way in.
    const bool trig = in.hand[gh].trigger > 0.6f;
    comboTimer_ = std::max(0.0f, comboTimer_ - dt);
    if (!firstPerson_ && trig && !swipeTrigWas_ && swipeT_ < 0 && player_.onGround) {
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
        }
        if (swipeT_ > dur || !player_.inAction()) swipeT_ = -1;
    }
    // Fire bolt: the move hand's trigger (first person: from that hand where it points; third
    // person: from her left hand toward the crosshair or straight ahead).
    boltCooldown_ -= dt;
    const bool boltTrig = in.hand[mh].trigger > 0.6f;
    if (boltTrig && !boltTrigWas_ && boltCooldown_ <= 0) {
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
                if (player_.onGround && swipeT_ < 0) player_.startAction(PlayerAnim::Cast, std::max(0.3f, player_.oneShot[int(PlayerAnim::Cast)] * 0.6f));
            }
        }
        if (ok) {
            Rocket r;
            r.pos = from;
            r.vel = dir * 28.0f;
            r.life = 0;
            r.kind = 1;
            rockets_.push_back(r);
            boltCooldown_ = 1.0f;
            sfx("sr_spfsh", from, 1.0f);
            haptics_.push_back({mh, 0.6f, 0.08f});
        }
    }
    boltTrigWas_ = boltTrig;
}

}  // namespace gvr
