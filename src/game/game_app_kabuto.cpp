// GameApp: Kabuto, the giant (the Kabuto story's hero) - his model and clips, the giant's scale
// (the user's tracking space scaled up to his eye height), punches, stomps, eating and roaring.
// Same class; see game_app.h ("Kabuto").
#include "game/game_app.h"

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
    // His clips for each PlayerAnim (no jetpack: he jumps; Inject = eating, Attack1/2 = right and
    // left punches, Attack3 = the stomp, Cast = the roar).
    static const char* clips[int(PlayerAnim::Count)] = {
        "kb_stand.anm",     "kb_walk.anm",       "kb_jump_up_loop.anm", "kb_stand.anm",   "kb_land_light.anm",
        "kb_side_r.anm",    "kb_slide.anm",      "kb_run_jump_loop.anm", "kb_jump_up_loop.anm", "kb_jump_up_loop.anm",
        "kb_jump_up_loop.anm", "kb_land.anm",    "kb_fall.anm",         "kb_eat_small.anm", "kb_mud_walk.anm",
        "kb_run1.anm",      "kb_punch.anm",      "kb_punch_lft.anm",    "kb_stomp.anm",   "kb_roar.anm"};
    for (int i = 1; i < int(PlayerAnim::Count); ++i) kabutoClip_[i] = std::max(0, m->addAnimation(assetWorld_->fs(), clips[i]));
    kabutoClip_[0] = 0;
    for (PlayerAnim a : {PlayerAnim::Land, PlayerAnim::LandHard, PlayerAnim::Inject, PlayerAnim::Attack1, PlayerAnim::Attack2,
                         PlayerAnim::Attack3, PlayerAnim::Cast})
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
        kabutoEyeModel_ = {0.5f * (hmn.x + hmx.x), hmn.y + 0.55f * (hmx.y - hmn.y), hmn.z - 0.2f};
    } else {
        kabutoEye_ = 0.85f * height;
        kabutoEyeModel_ = {0, kabutoEye_ - kabutoFeet_, -1.0f};
    }
    // First person (as the Meccaryn and Delphi): his head hidden, his arms reaching for the
    // controllers. Chain L_shoulder > L-arm > L_wrist (the forearm) > L_hand; two fingers and a
    // thumb on the right, two on the left (index side first).
    m->setHiddenBones({"head", "neck"});
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
            t.maxReachStretch = 1.3f;
            solveArmIk(world, children, c, t);
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
    p.runSpeed = b.runSpeed * g;
    p.strideSpeed = b.strideSpeed * g;
    p.turboStride = b.turboStride * g;
    p.strafeStride = b.strafeStride * g;
    p.jumpSpeed = b.jumpSpeed * std::sqrt(g);
    p.stepDown = b.stepDown * g;
    p.maxStepUp = b.maxStepUp * g;
    player_.params = p;
    heroScale_ = kbBaseScale_ * g;
    heroLift_ = kabutoFeet_ * g;
    const float H = kbBaseH_ * g;
    player_.height = H;
    player_.radius = 0.11f * H;
    player_.stepUp = 0.3f * heroScale_;
    player_.floatDepth = 0.7f * H;
}

void GameApp::kabutoAte(int what) {
    if (hero_ != Hero::Kabuto || kbGrow_ >= 1.0f) return;
    const float add = what == 0 ? 0.08f : (what == 1 ? 0.05f : 0.03f);
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
    if (hero_ == Hero::Kabuto && playMode_ && !cutsceneActive() && load_.phase == LoadPhase::Idle)
        s = std::max(1.0f, kabutoEye_ * kbGrowShown_ / std::max(1.0f, userEye_));
    if (const char* e = std::getenv("GVR_WORLD_SCALE")) s = float(std::atof(e));  // test hook
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

void GameApp::updateKabuto(float dt, const InputFrame& in) {
    gunValid_ = false;
    swordValid_ = false;
    tpAiming_ = false;
    if (!playerModel_ || !weapon_.onHit) return;
    KabutoState& k = kb_;
    const int gh = rig_.turnHand(), mh = rig_.moveHand();
    const float H = kMeccHeight * heroScale_;   // his height (m)
    const float S = rig_.comfort.worldScale;
    const Quat body = Quat::fromAxisAngle({0, 1, 0}, player_.yaw);
    const Vec3 fwd = body.rotate(Vec3{0, 0, -1});
    auto ground = [&](const Vec3& p) { return player_.groundHeight ? player_.groundHeight(p.x, p.z) : player_.position.y; };
    // A blow at a point: enemies, creatures and scenery within `r`.
    auto smash = [&](const Vec3& at, const Vec3& dir, float r, float mul) {
        bool any = false;
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
        // Smarties (village folk): squashed.
        auto& vs = villagers_.villagers;
        for (size_t i = 0; i < vs.size();) {
            if (length(vs[i].pos + Vec3{0, 0.5f, 0} - at) < r + 0.4f) {
                bleed(vs[i].pos + Vec3{0, 0.5f, 0}, dir, true, 0.7f);
                blood_.pool(vs[i].pos, 0.8f);
                vs.erase(vs.begin() + long(i));
                any = true;
            } else {
                ++i;
            }
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
        if (!firstPerson_ || !c.gripPoseValid || dt <= 0) { k.fistValid[h] = false; continue; }
        const Vec3 fist = rig_.toWorld(c.gripPose).position;
        if (k.fistValid[h] && k.fistCool[h] <= 0) {
            const Vec3 v = (fist - k.fistPrev[h]) * (1.0f / dt);
            const float real = length(v) / std::max(1.0f, S);  // the user's real hand speed (m/s)
            if (real > 2.2f) {
                const Vec3 dir = normalize(v);
                const float r = 0.10f * H;  // fist size
                const float mul = std::min(7.0f, 1.5f + 1.5f * real);
                bool hit = false;
                for (int s = 0; s <= 2 && !hit; ++s) hit = smash(k.fistPrev[h] + (fist - k.fistPrev[h]) * (0.5f * float(s)), dir, r, mul);
                if (hit) {
                    k.fistCool[h] = 0.35f;
                    haptics_.push_back({h, 1.0f, 0.12f});
                } else if (dir.y < -0.7f && std::min(fist.y - ground(fist), fist.y - player_.position.y) < 0.1f * H && real > 2.8f) {
                    LOGI("Kabuto: ground pound (hand %d, %.1f m/s)", h, real);
                    kabutoQuake(Vec3{fist.x, ground(fist), fist.z}, 0.3f * H, 45.0f);
                    k.fistCool[h] = 0.8f;
                }
            }
        }
        k.fistPrev[h] = fist;
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

    // --- X: grab and eat the nearest Reaper or creature in front (heals him).
    const bool x = in.hand[mh].primary;
    if (x && !k.xWas && !busy && player_.onGround) {
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
            if (vimps_.vimps[i].state != VimpState::Dead) consider(kVimpTargetK + int(i), vimps_.vimps[i].pos);
        if (best >= 0) {
            player_.startAction(PlayerAnim::Inject, std::max(0.5f, player_.oneShot[int(PlayerAnim::Inject)]));
            k.eatT = 0;
            k.eatDone = false;
            k.eatTarget = best;
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
            // Gone in one bite: killed and taken (no body left).
            const Vec3 mouth = player_.position + Vec3{0, 0.8f * H, 0} + fwd * (0.1f * H);
            bool ate = false;
            if (k.eatTarget >= kVimpTargetK) {
                const size_t vi = size_t(k.eatTarget - kVimpTargetK);
                if (vi < vimps_.vimps.size() && vimps_.vimps[vi].state != VimpState::Dead) {
                    vimps_.vimps[vi].remove = true;
                    vimps_.vimps[vi].state = VimpState::Dead;
                    vimps_.vimps[vi].slot = -1;
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
            } else if (size_t(k.eatTarget) < enemies_.enemies.size()) {
                Enemy& e = enemies_.enemies[size_t(k.eatTarget)];
                if (e.state != EnemyState::Dead) {
                    const int obj = e.objective;
                    if (enemies_.damage(k.eatTarget, 1e5f) && obj >= 0) completeObjective(obj);
                    enemies_.enemies[size_t(k.eatTarget)].remove = true;
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
    const bool roarNow = !y && k.yWas && kbYHold_ < 0.5f && !kbYLaid_;
    if (!y) { kbYHold_ = 0; kbYLaid_ = false; }
    if (roarNow && k.roarCool <= 0 && !busy && player_.onGround) {
        player_.startAction(PlayerAnim::Cast, std::min(3.0f, std::max(0.5f, player_.oneShot[int(PlayerAnim::Cast)])));
        static const char* roars[] = {"ka_roar1", "ka_roar2", "ka_roar3", "ka_roar4"};
        sfx2D(roars[int(frand() * 3.99f)], 1.0f);
        k.roarCool = 6.0f;
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

void GameApp::beginUiFrame(const EyeRenderInfo& eye) {
    pushUnscaledRig(lastInput_.head, lastInput_.headValid);
    if (!uiFrame_) return;
    const Pose eyeWorld = rig_.toWorld(eye.trackingPose);
    ViewParams v;
    v.view = Mat4::fromPose(eyeWorld.inverse());
    v.projection = Mat4::projectionFov(eye.fovLeft, eye.fovRight, eye.fovUp, eye.fovDown, 0.05f, 0.0f);
    v.eyePosition = eyeWorld.position;
    renderer_.beginView(v, eye.width, eye.height, false);
}

void GameApp::endUiFrame() { popUnscaledRig(); }

}  // namespace gvr
