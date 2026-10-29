// GameApp, rework (user, 2026-10-25: "make him also pick everything except buildings, that means
// trees, npcs"): what Kabuto carries in one hand besides boulders and Reapers (game_app_kbthrow.cpp).
// - Small folk (Smarties, Vimps, small creatures): held in the hand - bring it up to the mouth to
//   eat it, throw it (it dies where it lands), or open the hand gently to set it down.
// - Trees: uprooted whole; swung, the trunk clubs whatever it meets; thrown, it crashes down where it
//   lands; let go gently, it falls and is gone.
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/arm_ik.h"
#include "game/destruction.h"
#include "scene/asset_world.h"

namespace gvr {

namespace {
constexpr int kCreatureTargetC = 1000;  // as game_app_kabuto.cpp's target ranges
constexpr int kVillagerTargetC = 5000;
constexpr int kVimpTargetC = 6000;
constexpr float kGravityC = 14.0f;
}  // namespace

// Puts a carried victim at `at` (held / flying), or - when it lands - kills it (thrown) or sets it down.
void GameApp::dropVictim(int target, const Vec3& at, const Vec3& vel, bool thrown) {
    const Vec3 dir = length(vel) > 1e-3f ? normalize(vel) : Vec3{0, -1, 0};
    if (target >= kVimpTargetC) {
        const size_t i = size_t(target - kVimpTargetC);
        if (i >= vimps_.vimps.size()) return;
        vimps_.vimps[i].pos = at;
        if (thrown) vimps_.damage(int(i), 1e4f, at - dir);
    } else if (target >= kVillagerTargetC) {
        const size_t i = size_t(target - kVillagerTargetC);
        if (i >= villagers_.villagers.size()) return;
        villagers_.villagers[i].pos = at;
        if (thrown) hitVillager(int(i), 1000.0f, at + Vec3{0, 0.4f, 0}, dir);  // (its body lies where it landed)
        else if (villagers_.villagers[i].state != VillagerState::Held) villagers_.hold(int(i), true);  // squirming in his hand
    } else if (target >= kCreatureTargetC) {
        const size_t i = size_t(target - kCreatureTargetC);
        if (i >= creatures_.creatures.size()) return;
        creatures_.creatures[i].pos = at;
        if (thrown) {
            const int slot = creatures_.creatures[i].slot;
            if (creatures_.damage(int(i), 1e5f, dir) && slot >= 0 && size_t(slot) < chars_.size() && chars_[size_t(slot)].objective >= 0)
                completeObjective(chars_[size_t(slot)].objective);
        }
    }
    if (thrown) sfx("ka_stomp", at, 0.6f, 1.5f);
}

void GameApp::updateKabutoCarry(float dt, const InputFrame& in) {
    if (hero_ != Hero::Kabuto || !assetWorld_) { kbFlying_.clear(); return; }
    static bool listed = false;  // (GVR_GRAB_TRACE: the trees near him at the start)
    if (!listed && std::getenv("GVR_GRAB_TRACE")) {
        listed = true;
        for (const auto& it : destruct_.items)
            if (it.kind == DestructKind::Tree && it.alive() && length(it.base - player_.position) < 80.0f)
                LOGI("grab trace: tree at game (%.0f, %.0f), %.1f m tall, %.0f m away", -it.base.x / 0.2f, it.base.z / 0.2f, it.height, length(it.base - player_.position));
    }
    const float H = player_.height;
    const float S = std::max(1.0f, gameScale());  // (reach and throws: not shrunk by KABUTO VIEW: LOWER)
    auto ground = [&](const Vec3& p) { return player_.groundHeight ? player_.groundHeight(p.x, p.z) : player_.position.y; };
    for (auto& c : kbClubCool_) c.second -= dt;
    kbClubCool_.erase(std::remove_if(kbClubCool_.begin(), kbClubCool_.end(), [](const std::pair<uint32_t, float>& c) { return c.second <= 0; }),
                      kbClubCool_.end());
    if ((kbHold_ == HoldTree || kbHold_ == HoldVictim) && kbGrabHand_ >= 0) {
        const ControllerState& c = in.hand[kbGrabHand_];
        const bool valid = c.gripPoseValid && firstPerson_ && playMode_ && !dying_;
        const Pose g = rig_.toWorld(c.gripPose);
        const Pose a = c.aimPoseValid ? rig_.toWorld(c.aimPose) : g;
        const Vec3 hand = g.position + g.orientation.rotate(Vec3{0, 0.02f * H, -0.09f * H});
        const Vec3 handVel = kbHandVel_;  // (game_app_kbthrow.cpp tracks the grabbing hand's speed)
        const bool letGo = !valid || c.squeeze < 0.35f;
        const bool thrown = letGo && valid && length(handVel) / S > 1.2f;
        Vec3 vel = handVel * 1.5f;
        if (length(vel) > 90.0f) vel = vel * (90.0f / length(vel));
        if (kbHold_ == HoldVictim) {
            const int t = targetOfUid(kbHeld_, kbHeldUid_);
            if (t < 0) {
                kbHold_ = HoldNone;
            } else if (in.headValid && length(hand - (rig_.toWorld(in.head).position - Vec3{0, 0.06f * H, 0})) < 0.22f * H && kb_.eatT < 0) {
                LOGI("Kabuto: brought to his mouth (target %d)", t);
                kabutoEat(t);
                haptics_.push_back({kbGrabHand_, 0.8f, 0.2f});
                kbHold_ = HoldNone;
            } else if (letGo) {
                if (thrown) {
                    KbFlying f;
                    f.target = t;
                    f.uid = kbHeldUid_;
                    f.pos = hand;
                    f.vel = vel;
                    kbFlying_.push_back(f);
                    sfx2D("ka_throw", 1.0f);
                    haptics_.push_back({kbGrabHand_, 1.0f, 0.15f});
                    LOGI("Kabuto throws target %d (%.0f m/s)", t, length(vel));
                } else {
                    dropVictim(t, Vec3{hand.x, ground(hand), hand.z}, Vec3{0, 0, 0}, false);
                    if (t >= kVillagerTargetC && t < kVimpTargetC) villagers_.hold(t - kVillagerTargetC, false);
                    LOGI("Kabuto sets target %d down", t);
                }
                kbHold_ = HoldNone;
            } else {
                dropVictim(t, hand - Vec3{0, 0.04f * H, 0}, Vec3{0, 0, 0}, false);  // in his hand
            }
        } else {  // HoldTree
            const Vec3 axis = normalize(a.orientation.rotate(Vec3{0, 0, -1}));
            kbTreeAxis_ = axis;
            const Destructible& it = destruct_.items[size_t(kbHeld_)];
            const Vec3 tip = hand + axis * (0.85f * it.height);
            // A club: the trunk's sweep hits what it meets (each at most twice a second).
            if (kbTreeTipValid_ && dt > 0 && length(tip - kbTreeTipPrev_) / dt / S > 2.0f) {
                const Vec3 swing = normalize(tip - kbTreeTipPrev_);
                auto nearSeg = [&](const Vec3& p) {
                    const Vec3 ab = tip - hand;
                    const float k = std::clamp(dot(p - hand, ab) / std::max(1e-4f, dot(ab, ab)), 0.0f, 1.0f);
                    return length(p - (hand + ab * k));
                };
                auto cooled = [&](uint32_t key) {
                    for (const auto& cc : kbClubCool_) if (cc.first == key) return false;
                    kbClubCool_.push_back({key, 0.5f});
                    return true;
                };
                for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
                    const Enemy& e = enemies_.enemies[i];
                    if (e.state == EnemyState::Dead || nearSeg(e.pos + Vec3{0, 0.9f, 0}) > 1.2f + 0.04f * H || !cooled(e.uid)) continue;
                    hitMul_ = 4.0f;
                    weapon_.onHit(int(i), e.pos + Vec3{0, 0.9f, 0}, swing);
                    hitMul_ = 1.0f;
                    enemies_.push(int(i), swing * 14.0f + Vec3{0, 4, 0});
                    sfx("ex_woods", e.pos, 1.0f, 0.8f);
                    haptics_.push_back({kbGrabHand_, 0.9f, 0.1f});
                    LOGI("Kabuto clubs Reaper %u with the tree", e.uid);
                }
                for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
                    const Creature& cr = creatures_.creatures[i];
                    if (cr.state == CreatureState::Dead || CreatureDirector::underground(cr) || nearSeg(cr.pos + Vec3{0, 0.5f, 0}) > 1.5f + 0.04f * H ||
                        !cooled(0x80000000u | cr.uid))
                        continue;
                    hitMul_ = 4.0f;
                    if (weapon_.onHit) weapon_.onHit(kCreatureTargetC + int(i), cr.pos, swing);
                    hitMul_ = 1.0f;
                }
            }
            kbTreeTipPrev_ = tip;
            kbTreeTipValid_ = true;
            if (letGo) {
                KbFlying f;
                f.tree = true;
                f.item = kbHeld_;
                f.pos = hand;
                f.vel = thrown ? vel : Vec3{0, 0, 0};
                f.axis = axis;
                f.spin = Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} * (thrown ? 3.0f : 1.0f);
                kbFlying_.push_back(f);
                if (thrown) { sfx2D("ka_throw", 1.0f); haptics_.push_back({kbGrabHand_, 1.0f, 0.15f}); }
                LOGI("Kabuto %s the tree (%.0f m/s)", thrown ? "throws" : "drops", length(f.vel));
                kbHold_ = HoldNone;
            }
        }
        if (kbHold_ == HoldNone) { kbHeld_ = -1; kbHeldUid_ = 0; kbGrabHand_ = -1; }
    }
    // In flight: the small folk die where they land; trees crash down, hurting what is under them.
    for (auto& f : kbFlying_) {
        f.age += dt;
        f.vel.y -= kGravityC * dt;
        f.pos = f.pos + f.vel * dt;
        if (f.tree) {
            // Tumbling: the trunk's axis turns about the spin.
            const float sl = length(f.spin);
            if (sl > 1e-3f) f.axis = normalize(Quat::fromAxisAngle(f.spin * (1.0f / sl), sl * dt).rotate(f.axis));
            const Destructible& it = destruct_.items[size_t(f.item)];
            const Vec3 tip = f.pos + f.axis * (0.85f * it.height);
            const Vec3 low = tip.y < f.pos.y ? tip : f.pos;
            if (low.y <= ground(low) || f.age > 8.0f) {
                const Vec3 mid = (f.pos + tip) * 0.5f;
                if (length(f.vel) > 8.0f) explode(Vec3{mid.x, ground(mid) + 1.0f, mid.z}, 0.18f * H, 200.0f, false);
                dustBurst(Vec3{mid.x, ground(mid), mid.z}, 0.3f * it.height, 18);
                sfx("ex_treel", mid, 1.0f, 0.8f + 0.2f * frand());
                LOGI("The tree crashes down at game (%.0f %.0f)", -mid.x / 0.2f, mid.z / 0.2f);
                f.age = 99;
            }
            continue;
        }
        const int t = targetOfUid(f.target, f.uid);
        if (t < 0) { f.age = 99; continue; }
        f.target = t;
        const float g = ground(f.pos);
        if (f.pos.y <= g) {
            dropVictim(t, Vec3{f.pos.x, g, f.pos.z}, f.vel, true);
            LOGI("A thrown target lands and dies (%d)", t);
            f.age = 99;
        } else {
            dropVictim(t, f.pos, Vec3{0, 0, 0}, false);
        }
    }
    kbFlying_.erase(std::remove_if(kbFlying_.begin(), kbFlying_.end(), [](const KbFlying& f) { return f.age >= 99.0f; }), kbFlying_.end());
}

void GameApp::renderKabutoCarry() {
    if (hero_ != Hero::Kabuto || !assetWorld_) return;
    auto drawTree = [&](int item, const Vec3& at, const Vec3& axis) {
        if (item < 0 || size_t(item) >= destruct_.items.size()) return;
        const auto& o = assetWorld_->worldObjects()[size_t(destruct_.items[size_t(item)].object)];
        if (!o.model) return;
        const RenderModel& m = *o.model;
        const Vec3 c{(m.boundsMin.x + m.boundsMax.x) * 0.5f, m.boundsMin.y, (m.boundsMin.z + m.boundsMax.z) * 0.5f};
        const float h = (m.boundsMax.y - m.boundsMin.y) * o.scale;
        drawModel(o.model, Mat4::translation(at - axis * (0.12f * h)) * Mat4::rotation(rotationBetween(Vec3{0, 1, 0}, axis)) *
                               Mat4::scale(Vec3{o.scale, o.scale, o.scale}) * Mat4::translation(c * -1.0f), false);
    };
    if (kbHold_ == HoldTree && kbGrabHand_ >= 0 && lastInput_.hand[kbGrabHand_].gripPoseValid) {
        const Pose g = rig_.toWorld(lastInput_.hand[kbGrabHand_].gripPose);
        const float H = player_.height;
        drawTree(kbHeld_, g.position + g.orientation.rotate(Vec3{0, 0.02f * H, -0.09f * H}), kbTreeAxis_);
    }
    for (const auto& f : kbFlying_)
        if (f.tree) drawTree(f.item, f.pos, f.axis);
}

}  // namespace gvr
