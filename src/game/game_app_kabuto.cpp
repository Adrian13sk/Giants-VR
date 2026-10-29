// GameApp: Kabuto, the giant (the Kabuto story's hero) - his model and clips, the giant's scale
// (the user's tracking space scaled up to his eye height), punches, stomps, eating and roaring.
// Same class; see game_app.h ("Kabuto").
#include "game/game_app.h"
#include "game/hero_clips.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/arm_ik.h"
#include "game/cutscene.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"
#include "game/destruction.h"

namespace gvr {

namespace {
constexpr int kCreatureTargetK = 1000;  // as game_app.cpp's kCreatureTarget
constexpr int kVillagerTargetK = 5000;  // eat targets: Smarties of a village
constexpr int kVimpTargetK = 6000;      // eat targets: Vimps
constexpr float kMeccHeight = 1.8f;     // the Meccaryn's capsule height (m): heroScale_ = 1
}

bool GameApp::loadKabuto() {
    if (kabutoModel_) return true;
    if (!assetWorld_) return false;
    auto m = std::make_unique<SkinnedModel>();
    if (!m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "kb_l0", "kb_stand.anm")) return false;
    // His clips for each PlayerAnim (hero_clips.h).
    const char* const* clips = kKabutoClips;
    for (int i = 1; i < int(PlayerAnim::Count); ++i) kabutoClip_[i] = std::max(0, m->addAnimation(assetWorld_->fs(), clips[i]));
    kabutoClip_[0] = 0;
    for (PlayerAnim a : {PlayerAnim::Land, PlayerAnim::LandHard, PlayerAnim::Inject, PlayerAnim::Attack1, PlayerAnim::Attack2,
                         PlayerAnim::Attack3, PlayerAnim::Cast, PlayerAnim::Throw})
        kabutoOneShot_[int(a)] = m->animDuration(kabutoClip_[int(a)]);
    const int die = m->addAnimation(assetWorld_->fs(), "kb_die.anm");
    const int drown = m->addAnimation(assetWorld_->fs(), "kb_drown.anm");
    heroDeath_[int(Hero::Kabuto)][0] = die;
    heroDeath_[int(Hero::Kabuto)][1] = drown >= 0 ? drown : die;
    heroDeath_[int(Hero::Kabuto)][2] = die;
    // His size from the idle pose: feet on the ground, the eye at the front of the head.
    Vec3 mn, mx;
    m->boundsAt(0, 0.0f, mn, mx);
    m->pose(renderer_, 0.0f);
    kabutoFeet_ = -mn.y;
    const float height = mx.y - mn.y;
    Vec3 hmn, hmx;
    if (m->boneVertexBounds("head", hmn, hmx)) {
        kabutoEye_ = hmn.y + 0.55f * (hmx.y - hmn.y) + kabutoFeet_;
        // The eye 2 m (model) ahead of the face: his shoulders sit at eye height either side of
        // the head, and with the eye at the face a forward punch put the whole upper arm across the
        // view (harness, first person); 2 m (~24 cm at his scale) keeps the arms lower in view.
        kabutoEyeModel_ = {0.5f * (hmn.x + hmx.x), hmn.y + 0.55f * (hmx.y - hmn.y), hmn.z - 2.0f};
    } else {
        kabutoEye_ = 0.85f * height;
        kabutoEyeModel_ = {0, kabutoEye_ - kabutoFeet_, -1.0f};
    }
    // First person (as the Meccaryn and Delphi): his head hidden, his arms reaching for the
    // controllers. Chain L_shoulder > L-arm > L_wrist (the forearm) > L_hand; two fingers and a
    // thumb on the right, two on the left (index side first).
    {
        std::vector<std::string> hb = SkinnedModel::headBones();
        if (const char* extra = std::getenv("GVR_KB_HIDE")) hb.push_back(extra);  // test hook: which bone is that piece?
        m->setHiddenBones(hb);
    }
    m->setKeepFull(true);
    m->pose(renderer_, 0.0f);
    kabutoEyeFromHead_ = kabutoEyeModel_ - m->hiddenCentroid();
    m->setPoseModifier([this](const AnmAnimation& clip, const std::vector<std::vector<int>>& children, std::vector<Mat4>& world) {
        if (!playMode_ || !firstPerson_ || hero_ != Hero::Kabuto) return;
        auto node = [&](const char* n) {
            auto it = clip.byName.find(n);
            return it == clip.byName.end() ? -1 : it->second;
        };
        static const char* names[2][4] = {{"l_shoulder", "l-arm", "l_wrist", "l_hand"}, {"r_shoulder", "r_arm", "r_wrist", "r_hand"}};
        static const char* fingers[2][4] = {{"l_f2_1", nullptr, nullptr, "l_f1_1"}, {"r_f3_1", "r_f2_1", nullptr, "r_f1_1"}};
        for (int h = 0; h < 2; ++h) {
            if (!armTargets_[h].valid) continue;
            ArmChain c;
            c.clavicle = node(names[h][0]);
            c.upper = node(names[h][1]);
            c.fore = node(names[h][2]);
            c.hand = node(names[h][3]);
            for (int f = 0; f < 4; ++f) c.finger[f] = fingers[h][f] ? node(fingers[h][f]) : -1;
            if (!c.valid()) continue;
            ArmTarget t;
            t.grip = armTargets_[h].grip;
            t.forward = armTargets_[h].forward;
            t.up = armTargets_[h].up;
            t.pole = Vec3{h == kLeft ? 0.7f : -0.7f, 0.4f, -1.0f} - normalize(t.up) * 0.5f;
            t.maxReachStretch = 1.9f;  // (rework: the hands always reach the controllers)
            const float res = solveArmIk(world, children, c, t);
            static const bool ikTrace = std::getenv("GVR_IK_TRACE") != nullptr;
            static int ikN = 0;
            if (ikTrace && (++ikN % 72) == 0) LOGI("ik: Kabuto hand %d misses the controller by %.3f m", h, res);
        }
    });
    m->pose(renderer_, 0.0f);
    heroScale_ = 1.0f;  // set by setHero
    LOGI("Kabuto loaded (kb_l0): posed bounds y %.1f..%.1f m (height %.1f m), eye %.1f m, feet lift %.2f m", mn.y, mx.y, height,
         kabutoEye_, kabutoFeet_);
    kabutoModel_ = std::move(m);
    return true;
}

// The giant's scale: the user's tracking space is scaled so their eyes are at Kabuto's (only in
// play; cutscenes, loading and the free camera are human scale).
void GameApp::applyKabutoGrowth(float g) {
    if (hero_ != Hero::Kabuto) return;
    kbGrowShown_ = g;
    const PlayerParams& b = kbBaseParams_;
    PlayerParams p = player_.params;  // keeps what was set since (stun and landing times)
    // Speed with the square root of his size: a half-grown Kabuto is still quick (71 %).
    p.runSpeed = b.runSpeed * std::sqrt(g);
    p.strideSpeed = b.strideSpeed * g;
    p.turboStride = b.turboStride * g;
    p.strafeStride = b.strafeStride * g;
    p.jumpSpeed = b.jumpSpeed * std::sqrt(g);
    p.stepDown = b.stepDown * g;
    p.maxStepUp = b.maxStepUp * g;
    p.slopeWindow = 0.35f * kbBaseH_ * g;  // rework: he steps over banks and ledges small beside him
    player_.params = p;
    heroScale_ = kbBaseScale_ * g;
    heroLift_ = kabutoFeet_ * g;
    const float H = kbBaseH_ * g;
    player_.height = H;
    player_.radius = 0.11f * H;
    player_.stepUp = 0.3f * heroScale_;
    player_.floatDepth = 0.7f * H;
}

bool GameApp::kabutoFeedInput(Vec2& move, float& viewYaw, float dt) {
    kbFeedX_ = false;
    static const char* feed = std::getenv("GVR_KABUTO_FEED");
    if (!feed || hero_ != Hero::Kabuto || !playMode_ || dying_) return false;
    const float want = float(std::atof(feed));
    kbFeedT_ += dt;
    if (kbGrow_ >= want - 1e-3f) {
        if (!kbFeedDone_) {
            kbFeedDone_ = true;
            LOGI("Kabuto feed: grown to %.0f%% in %.1f s", kbGrow_ * 100.0f, kbFeedT_);
        }
        return false;
    }
    // The nearest Smartie or Vimp he can eat (not one already in his hand).
    const Vec3 me = player_.position;
    float best = 1e9f;
    Vec3 to;
    auto flatDist = [&](const Vec3& p) { return std::sqrt((p.x - me.x) * (p.x - me.x) + (p.z - me.z) * (p.z - me.z)); };
    auto skipped = [&](const Vec3& p) {
        for (const auto& s : kbFeedSkip_)
            if (length(Vec3{p.x - s.x, 0, p.z - s.z}) < 3.0f) return true;
        return false;
    };
    for (const auto& v : villagers_.villagers)
        if (v.state != VillagerState::Dead && v.state != VillagerState::Held && flatDist(v.pos) < best && !skipped(v.pos)) { best = flatDist(v.pos); to = v.pos; }
    for (const auto& v : vimps_.vimps)
        if (VimpDirector::killable(v) && flatDist(v.pos) < best && !skipped(v.pos)) { best = flatDist(v.pos); to = v.pos; }
    static bool said = false;
    if (best > 1e8f) {
        if (!said) { said = true; LOGI("Kabuto feed: nothing left to eat"); }
        return false;
    }
    const float H = player_.height;
    if (int(kbFeedT_ / 3.0f) != int((kbFeedT_ - dt) / 3.0f))
        LOGI("Kabuto feed: %.1f s, size %.0f%%, next meal %.1f m away (reach %.1f), at (%.1f %.1f)", kbFeedT_, kbGrow_ * 100.0f, best, 0.38f * H, me.x, me.z);
    viewYaw = std::atan2(-(to.x - me.x), -(to.z - me.z));
    if (best > 0.38f * H) {
        move = Vec2{0, 1};
        // No headway for 2 s (a house, a cliff): that meal is passed over.
        kbFeedStuckT_ += dt;
        if (length(me - kbFeedLast_) > 0.8f) { kbFeedLast_ = me; kbFeedStuckT_ = 0; }
        if (kbFeedStuckT_ > 2.0f) {
            kbFeedSkip_.push_back(to);
            kbFeedStuckT_ = 0;
            LOGI("Kabuto feed: can't get to the meal at (%.1f %.1f) - another", to.x, to.z);
        }
    } else {
        move = Vec2{0, 0};
        player_.yaw = viewYaw;                         // (face it: X takes what's in front)
        kbFeedX_ = std::fmod(kbFeedT_, 0.6f) < 0.12f;  // a tap now and then
    }
    return true;
}

void GameApp::kabutoAte(int what) {
    if (hero_ != Hero::Kabuto || kbGrow_ >= 1.0f) return;
    // A Smartie, a Vimp, anything else (Reapers, creatures): quicker growth than before (rework).
    const float add = what == 0 ? 0.10f : (what == 1 ? 0.07f : 0.05f);
    kbGrow_ = std::min(1.0f, kbGrow_ + add);
    timmyLine_ = kbGrow_ >= 1.0f ? std::string("KABUTO: FULL STRENGTH AND SIZE!") : "KABUTO GROWS: " + std::to_string(int(kbGrow_ * 100.0f + 0.5f)) + "%";
    timmyLineT_ = 2.5f;
    if (kbGrow_ >= 1.0f) {
        static const char* roars[] = {"ka_roar1", "ka_roar2"};
        sfx2D(roars[int(frand() * 1.99f)], 1.0f);
    }
    LOGI("Kabuto grows to %.0f%% of his full size", kbGrow_ * 100.0f);
}

void GameApp::updateHeroScale() {
    float s = 1.0f;
    if (hero_ == Hero::Kabuto && std::fabs(kbGrowShown_ - kbGrow_) > 1e-4f) {
        // Grows smoothly over about a second.
        const float step = 0.0012f;  // per frame: ~1 s per Smartie at 72 Hz
        applyKabutoGrowth(kbGrowShown_ + std::max(-step, std::min(step, kbGrow_ - kbGrowShown_)));
    }
    float full = 1.0f;
    if (hero_ == Hero::Kabuto && playMode_ && !cutsceneActive() && load_.phase == LoadPhase::Idle) {
        full = std::max(1.0f, kabutoEye_ * kbGrowShown_ / std::max(1.0f, userEye_));
        s = std::max(1.0f, full * (settings_.kabutoView == 1 ? 0.6f : 1.0f));  // (Options -> KABUTO VIEW)
    }
    if (const char* e = std::getenv("GVR_WORLD_SCALE")) s = full = float(std::atof(e));  // test hook
    // LOWER is a view: his body and hands look smaller, but he throws as hard and reaches as far
    // (see gameScale()). 2026-10-29 review: it had weakened his throws by 40 %.
    kbViewFactor_ = s / std::max(1.0f, full);
    // A big jump (the option changed in the menu): the eyes are held while the menu is up, but the
    // floor comes back under his feet on resume - in one frame, metres. Fade through black then.
    if (menuOpen_ && std::fabs(s - rig_.comfort.worldScale) > 0.1f * std::max(1.0f, rig_.comfort.worldScale)) kbViewJump_ = true;
    if (kbViewJump_ && !menuOpen_) {
        kbViewJump_ = false;
        respawnFade_ = std::max(respawnFade_, 1.0f);
    }
    if (std::fabs(s - scaleLogged_) > 0.25f) {  // (logged when it settles somewhere new: a test can read it)
        scaleLogged_ = s;
        LOGI("View scale %.2f (%s)", s, hero_ == Hero::Kabuto ? (settings_.kabutoView == 1 ? "Kabuto, lower view" : "Kabuto, giant view") : "life size");
    }
    if (s != rig_.comfort.worldScale && lastInput_.headValid) {
        // Keep the eyes where they are in the world while the scale changes (a cutscene starting
        // after Kabuto's scale was set had put the viewer 5 m into the ground).
        const Vec3 before = rig_.toWorld(lastInput_.head).position;
        rig_.comfort.worldScale = s;
        rig_.worldFromTracking.position = rig_.worldFromTracking.position + (before - rig_.toWorld(lastInput_.head).position);
    }
    rig_.comfort.worldScale = s;
}

void GameApp::kabutoQuake(const Vec3& at, float radius, float damage) {
    if (kbAdrenT_ > 0) { radius *= 1.4f; damage *= 2.0f; }  // adrenaline
    explode(at, radius, damage, /*fire=*/false);
    dustBurst(at, radius * 0.6f, 22);
    sfx("ka_stomp", at, 1.0f, 0.9f + 0.15f * frand());
    haptics_.push_back({kLeft, 1.0f, 0.25f});
    haptics_.push_back({kRight, 1.0f, 0.25f});
}

int GameApp::enemyOfUid(uint32_t uid) const {
    if (uid == 0) return -1;
    for (size_t i = 0; i < enemies_.enemies.size(); ++i)
        if (enemies_.enemies[i].uid == uid) return int(i);
    return -1;
}

uint32_t GameApp::targetUid(int t) const {
    if (t >= kVimpTargetK) return size_t(t - kVimpTargetK) < vimps_.vimps.size() ? vimps_.vimps[size_t(t - kVimpTargetK)].uid : 0;
    if (t >= kVillagerTargetK)
        return size_t(t - kVillagerTargetK) < villagers_.villagers.size() ? villagers_.villagers[size_t(t - kVillagerTargetK)].uid : 0;
    if (t >= kCreatureTargetK)
        return size_t(t - kCreatureTargetK) < creatures_.creatures.size() ? creatures_.creatures[size_t(t - kCreatureTargetK)].uid : 0;
    return t >= 0 && size_t(t) < enemies_.enemies.size() ? enemies_.enemies[size_t(t)].uid : 0;
}

int GameApp::targetOfUid(int kindOf, uint32_t uid) const {
    if (uid == 0) return -1;
    if (kindOf >= kVimpTargetK) {
        for (size_t i = 0; i < vimps_.vimps.size(); ++i)
            if (vimps_.vimps[i].uid == uid) return kVimpTargetK + int(i);
    } else if (kindOf >= kVillagerTargetK) {
        for (size_t i = 0; i < villagers_.villagers.size(); ++i)
            if (villagers_.villagers[i].uid == uid) return kVillagerTargetK + int(i);
    } else if (kindOf >= kCreatureTargetK) {
        for (size_t i = 0; i < creatures_.creatures.size(); ++i)
            if (creatures_.creatures[i].uid == uid) return kCreatureTargetK + int(i);
    } else if (kindOf >= 0) {
        return enemyOfUid(uid);
    }
    return -1;
}

void GameApp::kabutoEat(int target) {
    player_.startAction(PlayerAnim::Inject, std::max(0.5f, player_.oneShot[int(PlayerAnim::Inject)]));
    kb_.eatT = 0;
    kb_.eatDone = false;
    kb_.eatTarget = target;
    kb_.eatUid = targetUid(target);  // the bite lands 0.3 of the clip later: by then the lists may have shrunk
}

void GameApp::updateKabuto(float dt, const InputFrame& in) {
    gunValid_ = false;
    swordValid_ = false;
    tpAiming_ = false;
    if (!playerModel_ || !weapon_.onHit) return;
    KabutoState& k = kb_;
    const int gh = rig_.turnHand(), mh = rig_.moveHand();
    const float H = kMeccHeight * heroScale_;   // his height (m)
    // Test hook: GVR_KB_FACE_BOSS turns him to face the nearest living mission boss each frame.
    static const bool faceBoss = std::getenv("GVR_KB_FACE_BOSS") != nullptr;
    if (faceBoss) {
        float best = 1e9f;
        for (const auto& c : chars_)
            if (c.boss && c.hp > 0 && length(c.pos - player_.position) < best) {
                best = length(c.pos - player_.position);
                player_.yaw = std::atan2(-(c.pos.x - player_.position.x), -(c.pos.z - player_.position.z));
            }
    }
    const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
    const Vec3 fwd = body.rotate(Vec3{0, 0, -1});
    auto ground = [&](const Vec3& p) { return player_.groundHeight ? player_.groundHeight(p.x, p.z) : player_.position.y; };
    // A blow at a point: enemies, creatures and scenery within `r`.
    auto smash = [&](const Vec3& at, const Vec3& dir, float r, float mul) {
        bool any = false;
        rockHit(at, r);
        if (kbAdrenT_ > 0) mul *= 2.5f;  // adrenaline
        for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
            const Enemy& e = enemies_.enemies[i];
            if (e.state == EnemyState::Dead) continue;
            const Vec3 c = e.pos + Vec3{0, 0.9f, 0};
            if (length(c - at) < r + 0.5f) { hitMul_ = mul; weapon_.onHit(int(i), c, dir); hitMul_ = 1.0f; enemies_.push(int(i), dir * 10.0f); any = true; }
        }
        for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
            const Creature& c = creatures_.creatures[i];
            if (c.state == CreatureState::Dead || CreatureDirector::underground(c)) continue;
            const Vec3 p = c.pos + Vec3{0, 0.5f * c.scale, 0};
            if (length(p - at) < r + 0.6f * c.scale) { hitMul_ = mul; weapon_.onHit(kCreatureTargetK + int(i), p, dir); hitMul_ = 1.0f; any = true; }
        }
        for (size_t i = 0; i < destruct_.items.size(); ++i) {
            const Destructible& it = destruct_.items[i];
            if (!it.alive()) continue;
            const float y = std::max(it.base.y, std::min(it.base.y + it.height, at.y));
            if (length(Vec3{it.base.x, y, it.base.z} - at) < r + 0.5f * it.radius) {
                damageObject(int(i), 40.0f * mul, true, at);
                any = true;
            }
        }
        for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
            const Vimp& v = vimps_.vimps[i];
            if (v.state != VimpState::Dead && length(v.pos + Vec3{0, 2.0f, 0} - at) < r + 1.5f) { hitVimp(int(i), 25.0f * mul, at, dir); any = true; }
        }
        for (size_t i = 0; i < chars_.size(); ++i) {  // mission targets: the other Kabuto, Offspring...
            const CharacterInst& c = chars_[i];
            if (!c.boss || c.hp <= 0) continue;
            const float y = std::max(c.pos.y, std::min(c.pos.y + c.bossH, at.y));
            if (length(Vec3{c.pos.x, y, c.pos.z} - at) < r + c.bossR) { hitBoss(int(i), 30.0f * mul, at, dir); any = true; }
        }
        // Skirmish players.
        if (skirmish_)
            for (const auto& o : sk_.players) {
                if (!o.alive || o.local || !sk_.enemies(o.id, skLocal_)) continue;
                const Vec3 c = o.body.position + Vec3{0, 0.5f * o.body.height, 0};
                if (length(c - at) < r + o.body.radius + 0.5f) { hitMul_ = mul; weapon_.onHit(kBotTarget + o.id, c, dir); hitMul_ = 1.0f; any = true; }
            }
        // Smarties (village folk): squashed.
        auto& vs = villagers_.villagers;
        for (size_t i = 0; i < vs.size(); ++i)
            if (VillagerDirector::alive(vs[i]) && length(vs[i].pos + Vec3{0, 0.5f, 0} - at) < r + 0.4f) {
                hitVillager(int(i), 1000.0f, vs[i].pos + Vec3{0, 0.5f, 0}, dir);  // (the body stays - rework)
                any = true;
            }
        if (any) {
            kabutoGain(0.03f);
            sfx(mul > 4.0f ? "ka_hvht1" : (mul > 2.5f ? "ka_mdht1" : "ka_ltht1"), at, 1.0f, 0.9f + 0.2f * frand());
            enemies_.alertNear(at, 40.0f);
        }
        return any;
    };

    // --- First person: his fists are the controllers. A fast swing smashes what it meets; a fast
    // blow down to the ground is a ground pound (a small quake).
    for (int h = 0; h < 2; ++h) {
        k.fistCool[h] = std::max(0.0f, k.fistCool[h] - dt);
        const ControllerState& c = in.hand[h];
        if (!firstPerson_ || !c.gripPoseValid || dt <= 0 || ((h == mh && kbHold_ != HoldNone && kbGrabHand_ < 0) || h == kbGrabHand_)) { k.fistValid[h] = false; continue; }  // (a hand carrying something throws it, doesn't punch)
        const Vec3 fist = rig_.toWorld(c.gripPose).position;
        // The swing is the user's own hand (tracking space): where the body went meanwhile (a respawn,
        // a snap turn, a fall) is not a punch. Its path is drawn in the world as the rig stands now.
        Pose prevPose = c.gripPose;
        prevPose.position = k.fistPrev[h];
        const Vec3 fistBefore = rig_.toWorld(prevPose).position;
        if (k.fistValid[h] && k.fistCool[h] <= 0) {
            const Vec3 v = (fist - fistBefore) * (1.0f / dt);
            const float real = length(c.gripPose.position - k.fistPrev[h]) / dt;  // the user's real hand speed (m/s)
            if (real > 2.2f) {
                const Vec3 dir = normalize(v);
                const float r = 0.10f * H;  // fist size
                const float mul = std::min(7.0f, 1.5f + 1.5f * real);
                bool hit = false;
                for (int s = 0; s <= 2 && !hit; ++s) hit = smash(fistBefore + (fist - fistBefore) * (0.5f * float(s)), dir, r, mul);
                if (hit) {
                    LOGI("Kabuto: fist blow (hand %d, %.1f m/s)", h, real);
                    k.fistCool[h] = 0.35f;
                    haptics_.push_back({h, 1.0f, 0.12f});
                } else if (dir.y < -0.7f && std::min(fist.y - ground(fist), fist.y - player_.position.y) < 0.1f * H && real > 2.8f) {
                    LOGI("Kabuto: ground pound (hand %d, %.1f m/s)", h, real);
                    kabutoQuake(Vec3{fist.x, ground(fist), fist.z}, 0.3f * H, 45.0f);
                    k.fistCool[h] = 0.8f;
                }
            }
        }
        k.fistPrev[h] = c.gripPose.position;
        k.fistValid[h] = true;
    }

    // --- The turn hand's trigger: a punch (third person; alternating right and left).
    const bool trig[2] = {in.hand[0].trigger > 0.6f, in.hand[1].trigger > 0.6f};
    const bool busy = player_.inAction() || k.punchT >= 0 || k.stompT >= 0 || k.eatT >= 0;
    if (!firstPerson_ && trig[gh] && !k.trigWas[gh] && !busy && player_.onGround && !kbHasGlass_) {  // (with the glass: it burns)
        k.punchLeft = !k.punchLeft;
        const PlayerAnim a = k.punchLeft ? PlayerAnim::Attack2 : PlayerAnim::Attack1;
        player_.startAction(a, std::max(0.3f, player_.oneShot[int(a)]));
        k.punchT = 0;
        k.punchHit = false;
        sfx2D(k.punchLeft ? "ka_pnch2" : "ka_pnch1", 0.9f);
    }
    if (k.punchT >= 0) {
        k.punchT += dt;
        const PlayerAnim a = k.punchLeft ? PlayerAnim::Attack2 : PlayerAnim::Attack1;
        const float dur = std::max(0.3f, player_.oneShot[int(a)]);
        if (!k.punchHit && k.punchT > dur * 0.45f) {
            k.punchHit = true;
            // The fist lands in front at chest-to-knee height: try a few points along the arc.
            bool hit = false;
            for (float d : {0.25f, 0.4f, 0.55f})
                for (float y : {0.15f, 0.45f})
                    if (!hit) hit = smash(player_.position + fwd * (d * H) + Vec3{0, y * H, 0}, fwd, 0.14f * H, 5.0f);
            if (hit) haptics_.push_back({gh, 1.0f, 0.15f});
            static const bool trace = std::getenv("GVR_KB_TRACE") != nullptr;
            if (trace) {
                LOGI("Kabuto punch (third person): %s", hit ? "hit" : "nothing in reach");
                for (const auto& c : chars_)
                    if (c.boss && c.hp > 0) LOGI("  boss at game (%.0f %.0f) %.0f hp, %.1f m away (radius %.1f, height %.1f)", -c.pos.x / 0.2f, c.pos.z / 0.2f, c.hp, length(Vec3{c.pos.x - player_.position.x, 0, c.pos.z - player_.position.z}), c.bossR, c.bossH);
                for (const auto& it : destruct_.items)
                    if (it.alive() && length(it.base - player_.position) < 3.0f * H)
                        LOGI("  object %d kind %d: %.0f/%.0f hp, %.1f m away (radius %.1f)", it.object, int(it.kind), it.hp, it.maxHp, length(Vec3{it.base.x - player_.position.x, 0, it.base.z - player_.position.z}), it.radius);
            }
        }
        if (k.punchT > dur) k.punchT = -1;
    }

    // --- The move hand's trigger: the stomp (both views) - a quake round his front foot.
    if (trig[mh] && !k.trigWas[mh] && !busy && player_.onGround) {
        player_.startAction(PlayerAnim::Attack3, std::max(0.4f, player_.oneShot[int(PlayerAnim::Attack3)]));
        k.stompT = 0;
        k.stompHit = false;
    }
    if (k.stompT >= 0) {
        k.stompT += dt;
        const float dur = std::max(0.4f, player_.oneShot[int(PlayerAnim::Attack3)]);
        if (!k.stompHit && k.stompT > dur * 0.5f) {
            k.stompHit = true;
            Vec3 at = player_.position + fwd * (0.15f * H);
            at.y = ground(at);
            kabutoQuake(at, 0.45f * H, 90.0f);
        }
        if (k.stompT > dur) k.stompT = -1;
    }
    k.trigWas[0] = trig[0];
    k.trigWas[1] = trig[1];

    // --- X: grab and eat the nearest Smartie, Vimp or creature in front (heals him). A Reaper or a
    // boulder is grabbed and carried instead (updateThrows): tapped, a Reaper is eaten; held, thrown.
    const bool x = in.hand[mh].primary || kbFeedX_;
    if (x && !k.xWas && !busy && player_.onGround && !boulderInReach()) {
        int best = -1;
        float bestD = 0.6f * H;
        auto consider = [&](int id, const Vec3& p) {
            Vec3 d = p - player_.position;
            d.y = 0;
            const float l = length(d);
            if (l < bestD && (l < 0.1f * H || dot(d * (1.0f / l), fwd) > 0.2f)) { bestD = l; best = id; }
        };
        for (size_t i = 0; i < enemies_.enemies.size(); ++i)
            if (enemies_.enemies[i].state != EnemyState::Dead) consider(int(i), enemies_.enemies[i].pos);
        for (size_t i = 0; i < creatures_.creatures.size(); ++i)
            if (creatures_.creatures[i].state != CreatureState::Dead && creatures_.creatures[i].scale < 2.5f &&
                !CreatureDirector::underground(creatures_.creatures[i]))
                consider(kCreatureTargetK + int(i), creatures_.creatures[i].pos);
        for (size_t i = 0; i < villagers_.villagers.size(); ++i) consider(kVillagerTargetK + int(i), villagers_.villagers[i].pos);
        for (size_t i = 0; i < vimps_.vimps.size(); ++i)
            if (VimpDirector::killable(vimps_.vimps[i])) consider(kVimpTargetK + int(i), vimps_.vimps[i].pos);
        if (best >= 0 && best < kCreatureTargetK) {
            // A Reaper: updateThrows grabs it.
        } else if (best >= 0) {
            kabutoEat(best);
            sfx2D("ka_grab", 0.9f);
            LOGI("Kabuto grabs target %d at %.1f m", best, bestD);
        } else {
            sfx2D("ka_sniff", 0.7f);
            LOGI("Kabuto: nothing to eat within %.1f m in front", 0.6f * H);
        }
    }
    k.xWas = x;
    if (k.eatT >= 0) {
        k.eatT += dt;
        const float dur = std::max(0.5f, player_.oneShot[int(PlayerAnim::Inject)]);
        if (!k.eatDone && k.eatT > dur * 0.3f) {
            k.eatDone = true;
            k.eatTarget = targetOfUid(k.eatTarget, k.eatUid);  // where it is in its list now (-1: gone meanwhile)
            // Gone in one bite: killed and taken (no body left).
            const Vec3 mouth = player_.position + Vec3{0, 0.8f * H, 0} + fwd * (0.1f * H);
            bool ate = false;
            if (k.eatTarget >= kVimpTargetK) {
                const size_t vi = size_t(k.eatTarget - kVimpTargetK);
                if (vi < vimps_.vimps.size() && vimps_.vimps[vi].state != VimpState::Dead) {
                    vimps_.eat(int(vi));  // (rework: no body; its soul stands there, and it is born again later)
                    ate = true;
                }
            } else if (k.eatTarget >= kVillagerTargetK) {
                const size_t vi = size_t(k.eatTarget - kVillagerTargetK);
                if (vi < villagers_.villagers.size()) {
                    villagers_.villagers.erase(villagers_.villagers.begin() + long(vi));
                    ate = true;
                }
            } else if (k.eatTarget >= kCreatureTargetK) {
                const size_t ci = size_t(k.eatTarget - kCreatureTargetK);
                if (ci < creatures_.creatures.size() && creatures_.creatures[ci].state != CreatureState::Dead) {
                    const int slot = creatures_.creatures[ci].slot;
                    if (creatures_.damage(int(ci), 1e5f, fwd) && slot >= 0 && size_t(slot) < chars_.size() && chars_[size_t(slot)].objective >= 0)
                        completeObjective(chars_[size_t(slot)].objective);
                    creatures_.creatures[ci].remove = true;
                    ate = true;
                }
            } else if (k.eatTarget >= 0 && size_t(k.eatTarget) < enemies_.enemies.size()) {
                Enemy& e = enemies_.enemies[size_t(k.eatTarget)];
                if (e.state != EnemyState::Dead) {
                    const int obj = e.objective;
                    if (enemies_.damage(k.eatTarget, 1e5f) && obj >= 0) completeObjective(obj);
                    enemies_.enemies[size_t(k.eatTarget)].remove = true;
                    ate = true;
                } else if (!e.gibbed && !e.remove && e.uid == k.eatUid) {
                    e.remove = true;  // (rework: a body he picked up and brought to his mouth)
                    ate = true;
                }
            }
            if (ate) {
                kabutoAte(k.eatTarget >= kVimpTargetK ? 1 : (k.eatTarget >= kVillagerTargetK ? 0 : 2));
                kabutoGain(0.15f);
                if (k.eatTarget >= kVillagerTargetK && k.eatTarget < kVimpTargetK) kabutoAteSmartie();
                playerHealth_ = std::min(100.0f, playerHealth_ + 25.0f);
                healFlash_ = 1.0f;
                blood_.burst(mouth, Vec3{0, -1, 0}, 14, 3.0f, 0.12f);
                if (k.eatTarget >= kVimpTargetK) sfx2D(frand() < 0.5f ? "ka_vimp1" : "ka_vimp2", 1.0f);
                else if (k.eatTarget >= kVillagerTargetK) sfx2D(frand() < 0.5f ? "ka_smrt1" : "ka_smrt2", 1.0f);  // (names read as "Smartie")
                else sfx2D(frand() < 0.5f ? "ka_eat1" : "ka_eat2", 1.0f);
                haptics_.push_back({mh, 0.7f, 0.2f});
                if (k.eatTarget < kVimpTargetK) spawnBones(mouth, 2 + int(frand() * 2.0f), fwd, 3.0f);  // (he spits out the bones)
                LOGI("Kabuto ate something: health %.0f", playerHealth_);
            }
        }
        if (k.eatT > dur) {
            k.eatT = -1;
            if (frand() < 0.3f) sfx2D("ka_belch", 0.8f);
        }
    }

    // --- Y: the roar. Reapers about are knocked back, creatures scatter.
    k.roarCool = std::max(0.0f, k.roarCool - dt);
    const bool y = in.hand[mh].secondary;
    // Held 0.6 s with Offspring to lay: an egg; a short press: the roar (on release).
    if (y) {
        kbYHold_ += dt;
        if (kbYHold_ > 0.6f && !kbYLaid_ && kbOffspringCharges_ > 0) { kbYLaid_ = true; layOffspring(); haptics_.push_back({mh, 0.6f, 0.2f}); }
    }
    bool roarNow = !y && k.yWas && kbYHold_ < 0.5f && !kbYLaid_;
    // Chest beat (rework, first person): a fist driven onto the chest (tracking space, the user's own
    // metres), left and right in turn, three blows each within 0.9 s of the last = the roar.
    kbBeatT_ += dt;
    if (firstPerson_ && in.headValid && dt > 0) {
        const Vec3 hf = in.head.orientation.rotate(Vec3{0, 0, -1});
        Vec3 flat{hf.x, 0, hf.z};
        flat = length(flat) > 1e-3f ? normalize(flat) : Vec3{0, 0, -1};
        const Vec3 chest = in.head.position + Vec3{0, -0.36f, 0} + flat * 0.1f;
        for (int h = 0; h < 2; ++h) {
            const ControllerState& c = in.hand[h];
            if (!c.gripPoseValid) { kbBeatPrevOk_[h] = false; continue; }
            const Vec3 p = c.gripPose.position;
            const float d = length(p - chest);
            const float closing = kbBeatPrevOk_[h] ? (length(kbBeatPrev_[h] - chest) - d) / dt : 0.0f;
            kbBeatPrev_[h] = p;
            kbBeatPrevOk_[h] = true;
            if (d > 0.3f) { kbBeatIn_[h] = false; continue; }
            if (kbBeatIn_[h] || d > 0.2f || closing < 0.9f) continue;
            kbBeatIn_[h] = true;
            kbBeatN_ = (kbBeatT_ < 0.9f && kbBeatLast_ == 1 - h) ? kbBeatN_ + 1 : 1;
            kbBeatLast_ = h;
            kbBeatT_ = 0;
            sfx2D("ka_pnch1", 0.6f, 0.75f + 0.1f * frand());  // a thump
            haptics_.push_back({h, 0.8f, 0.07f});
            LOGI("Kabuto beats his chest (%d)", kbBeatN_);
            if (kbBeatN_ >= 3) { roarNow = true; kbBeatN_ = 0; }
        }
    }
    if (!y) { kbYHold_ = 0; kbYLaid_ = false; }
    // (2026-10-28) Roar into the microphone (Options -> KABUTO MIC ROAR): loud for a quarter second = his roar,
    // the louder the wider it reaches. Test hook GVR_MIC_TEST=<level>,<from s>,<to s> stands in for the mic.
    {
        float level = (settings_.micRoar && micAllowed_) ? micLevel_ : 0.0f;
        static const char* mt = std::getenv("GVR_MIC_TEST");
        if (mt) {
            float lv = 0, a = 0, b = 0;
            if (std::sscanf(mt, "%f,%f,%f", &lv, &a, &b) == 3) level = (float(time_) >= a && float(time_) < b) ? lv : 0.0f;
        }
        micLoudT_ = level > 0.12f ? micLoudT_ + dt : 0.0f;
        if (micLoudT_ > 0.25f && k.roarCool <= 0) {
            roarNow = true;
            micLoudT_ = 0;
            LOGI("Kabuto roars into the microphone (level %.2f)", level);
        }
    }
    if (roarNow && k.roarCool <= 0 && !busy && player_.onGround) {
        player_.startAction(PlayerAnim::Cast, std::min(3.0f, std::max(0.5f, player_.oneShot[int(PlayerAnim::Cast)])));
        static const char* roars[] = {"ka_roar1", "ka_roar2", "ka_roar3", "ka_roar4"};
        sfx2D(roars[int(frand() * 3.99f)], 1.0f);
        k.roarCool = 6.0f;
        LOGI("Kabuto roars");
        haptics_.push_back({kLeft, 0.6f, 0.6f});
        haptics_.push_back({kRight, 0.6f, 0.6f});
        const float R = 1.6f * H;
        for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
            Vec3 d = enemies_.enemies[i].pos - player_.position;
            d.y = 0;
            const float l = length(d);
            if (l < R && l > 1e-3f && enemies_.enemies[i].state != EnemyState::Dead) enemies_.push(int(i), d * (9.0f * (1.0f - l / R) / l));
        }
        enemies_.alertNear(player_.position, R);
    }
    k.yWas = y;

    // --- Walking into trees and Smartie houses tramples them (he is 3-4 times their height).
    if (player_.onGround && player_.horizontalSpeed() > 2.0f) {
        Vec3 dir = player_.velocity;
        dir.y = 0;
        dir = normalize(dir);
        for (size_t i = 0; i < destruct_.items.size(); ++i) {
            const Destructible& it = destruct_.items[i];
            if (!it.alive() || (it.kind != DestructKind::Tree && it.kind != DestructKind::House) || it.height > 0.7f * H) continue;
            Vec3 d = it.base - player_.position;
            d.y = 0;
            const float l = length(d);
            if (l < player_.radius + 0.5f * it.radius + 1.0f && (l < 1e-3f || dot(d * (1.0f / l), dir) > 0.0f)) {
                damageObject(int(i), 1e4f, true, player_.position);
                haptics_.push_back({kLeft, 0.5f, 0.1f});
                haptics_.push_back({kRight, 0.5f, 0.1f});
            }
        }
    }

    // --- A heavy landing shakes the ground.
    if (!player_.onGround && player_.velocity.y < -14.0f) k.wasAir = true;
    if (player_.onGround && k.wasAir) {
        k.wasAir = false;
        Vec3 at = player_.position;
        at.y = ground(at);
        kabutoQuake(at, 0.4f * H, 60.0f);
        sfx2D("ka_lndhd", 1.0f);
    }
}

}  // namespace gvr

namespace gvr {

void GameApp::pushUnscaledRig(const Pose& head, bool headValid) {
    uiFrame_ = false;
    if (!headValid || rig_.comfort.worldScale == 1.0f) return;
    uiScaleSaved_ = rig_.comfort.worldScale;
    uiRigSaved_ = rig_.worldFromTracking;
    // Same head position and direction in the world, tracking space at scale 1 about it.
    const Vec3 hw = rig_.toWorld(head).position;
    rig_.comfort.worldScale = 1.0f;
    rig_.worldFromTracking.position = hw - rig_.worldFromTracking.orientation.rotate(head.position);
    uiFrame_ = true;
}

void GameApp::popUnscaledRig() {
    if (!uiFrame_) return;
    rig_.comfort.worldScale = uiScaleSaved_;
    rig_.worldFromTracking = uiRigSaved_;
    uiFrame_ = false;
}

void GameApp::beginUiFrame(const EyeRenderInfo* eyes, int n) {
    pushUnscaledRig(lastInput_.head, lastInput_.headValid);
    if (!uiFrame_) return;
    beginEyeViews(eyes, n, nullptr, false);
}

void GameApp::endUiFrame() { popUnscaledRig(); }

}  // namespace gvr
