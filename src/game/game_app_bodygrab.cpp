// GameApp, rework: taking hold of bodies. The Meccaryn and Delphi reach down to a body lying on the
// ground (a dead Reaper, Ripper, Smartie or Vimp - a ragdoll), squeeze a grip with the hand on it and
// it hangs from the hand by the part taken: an arm, a leg, the head. It drags along the ground, swings
// as you turn, and flies when you let go with a throw. Our own design, on game/ragdoll.h.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/creature.h"
#include "game/game_app.h"
#include "game/villager.h"
#include "game/vimp.h"

namespace gvr {

namespace {
constexpr int kReaperBody = 3;      // heldBody_.kind beyond the NPC kinds (0 creature, 1 Smartie, 2 Vimp)
constexpr float kReach = 0.42f;     // m from the hand to the nearest part of the body
}  // namespace

Ragdoll* GameApp::bodyRagdoll(const HeldBody& b) {
    if (b.kind < 0 || b.slot < 0) return nullptr;
    if (b.kind == kReaperBody) {
        for (auto& e : enemies_.enemies)
            if (e.uid == b.uid) {
                if (e.state != EnemyState::Dead || e.gibbed || e.posted) return nullptr;
                Ragdoll* r = ragdollOf(e);
                return r && r->active ? r : nullptr;
            }
        return nullptr;
    }
    if (size_t(b.slot) >= npcRag_[b.kind].size()) return nullptr;
    Ragdoll& r = npcRag_[b.kind][size_t(b.slot)];
    return r.active && r.uid == b.uid ? &r : nullptr;
}

bool GameApp::thrownHitEnemy(int i, float damage, const Vec3& at, const Vec3& dir) {
    if (i < 0 || size_t(i) >= enemies_.enemies.size()) return false;
    const bool killed = enemies_.damage(i, damage);
    const Enemy& e = enemies_.enemies[size_t(i)];
    bleed(at, dir, killed, 0.6f);
    if (killed) {
        if (e.objective >= 0) completeObjective(e.objective);
        blood_.pool(e.pos, 1.0f);
        enemies_.alertNear(e.pos, 25.0f);  // a comrade falls
    }
    return killed;
}

bool GameApp::thrownHitCreature(int i, float damage, const Vec3& at, const Vec3& dir) {
    if (i < 0 || size_t(i) >= creatures_.creatures.size()) return false;
    std::vector<CreatureEvent> ev;
    const float scale = creatures_.creatures[size_t(i)].scale;
    const int slot = creatures_.creatures[size_t(i)].slot;
    const bool died = creatures_.damage(i, damage, dir, &ev);
    if (died && slot >= 0 && size_t(slot) < chars_.size() && chars_[size_t(slot)].objective >= 0) completeObjective(chars_[size_t(slot)].objective);
    bleed(at, dir, died, scale);
    if (died && !creatures_.creatures[size_t(i)].exploded) blood_.pool(creatures_.creatures[size_t(i)].pos, 0.9f * scale);
    for (const auto& e : ev)
        if (e.kind == CreatureEvent::Die && e.burst) {
            spawnGore(e.pos, e.dir, scale);
            blasts_.push_back({e.pos + Vec3{0, 0.5f * scale, 0}, 3, 0.0f});
        }
    for (const auto& e : ev) creatureSound(e);
    return died;
}

// A thrown body knocks into the living: a Reaper (or creature) it meets at speed is shoved and hurt.
void GameApp::updateThrownBody(float dt) {
    HeldBody& b = thrownBody_;
    if (b.kind < 0) return;
    b.t += dt;
    Ragdoll* r = bodyRagdoll(b);
    if (!r || b.t > 2.0f || dt <= 0) { b = HeldBody{}; return; }
    const Vec3 c = r->centre();
    const Vec3 v = (c - b.prev) * (1.0f / dt);
    b.prev = c;
    const float sp = length(v);
    static const bool trace = std::getenv("GVR_GRAB_TRACE") != nullptr;
    if (trace && int(b.t / dt) % 6 == 0) {
        float nearest = 1e9f;
        Vec3 at;
        for (const auto& e : enemies_.enemies)
            if (e.state != EnemyState::Dead && length(Vec3{e.pos.x - c.x, 0, e.pos.z - c.z}) < nearest) {
                nearest = length(Vec3{e.pos.x - c.x, 0, e.pos.z - c.z});
                at = e.pos;
            }
        LOGI("grab trace: thrown body at (%.2f %.2f %.2f) %.1f m/s, nearest living Reaper %.2f m at (%.2f %.2f %.2f)", c.x, c.y, c.z, sp, nearest, at.x, at.y, at.z);
    }
    if (sp < 2.5f || b.t < 0.05f) return;
    auto already = [&](uint32_t uid) {
        for (int k = 0; k < b.nHit; ++k)
            if (b.hit[k] == uid) return true;
        return b.nHit >= 4;
    };
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        Enemy& e = enemies_.enemies[i];
        if (e.state == EnemyState::Dead || e.uid == b.uid || already(e.uid)) continue;
        const Vec3 d{e.pos.x - c.x, 0, e.pos.z - c.z};
        if (length(d) > 1.0f || c.y < e.pos.y - 0.3f || c.y > e.pos.y + 2.4f) continue;
        b.hit[b.nHit++] = e.uid;
        const Vec3 dir = normalize(Vec3{v.x, 0, v.z} + Vec3{0.001f, 0, 0});
        enemies_.push(int(i), dir * std::min(8.0f, 1.2f * sp));
        const bool killed = thrownHitEnemy(int(i), 6.0f + 4.0f * sp, c, dir);
        r->kick(v * -0.4f);  // (the body stops against him)
        sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", c, std::min(1.0f, 0.4f + sp * 0.08f), 1.0f);
        LOGI("The thrown body hits Reaper %u at %.1f m/s%s", e.uid, sp, killed ? " - he's dead" : "");
    }
}

void GameApp::updateBodyGrab(float dt, const InputFrame& in) {
    updateThrownBody(dt);
    const bool can = playMode_ && firstPerson_ && !dying_ && !cutsceneActive() && !riding_;
    // Kabuto too (user: he picks up everything but buildings): a dead Reaper or creature is a doll in his
    // hand - bring it to his mouth and he eats it. His hand's point and reach go with his size.
    const bool kab = hero_ == Hero::Kabuto;
    const float H = player_.height;
    auto handAt = [&](const ControllerState& c) {
        const Pose gp = rig_.toWorld(c.gripPose);
        return kab ? gp.position + gp.orientation.rotate(Vec3{0, 0.02f * H, -0.09f * H}) : gp.position;
    };
    const float reach = kab ? 0.24f * H : kReach;  // (as his own grab)
    static const bool trace = std::getenv("GVR_GRAB_TRACE") != nullptr;
    // Taking hold: a grip squeezed (its edge) with the hand on a body, the other hand not holding one.
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        const bool g = c.squeeze > 0.7f;
        const bool edge = g && !bodyGripWas_[h];
        bodyGripWas_[h] = g;
        if (!can || !edge || heldBody_.kind >= 0 || !c.gripPoseValid || (heldVillager_ != 0 && heldVillagerHand_ == h) || stoneInHand(h) || climbHold_[h]) continue;
        if (kab && (kbHold_ != HoldNone || kbGrabHand_ == h || kb_.eatT >= 0 || climbHold_[h])) continue;  // (that hand has his own grab)
        const Vec3 p = kab ? handAt(c) : reachPoint(handAt(c));  // (GROUND REACH)
        float best = reach, nearestAny = 1e30f;
        HeldBody pick;
        int part = -1;
        for (auto& e : enemies_.enemies) {
            if (e.state != EnemyState::Dead || e.gibbed || e.posted || e.uid == impaledUid_) continue;
            if (length(e.pos - p) > 3.0f + reach) continue;
            Ragdoll* r = ragdollOf(e);
            if (!r || !r->active) continue;
            float d = 0;
            const int k = r->nearest(p, d);
            nearestAny = std::min(nearestAny, d);
            if (k >= 0 && d < best) { best = d; part = k; pick = HeldBody{kReaperBody, e.slot, e.uid, h}; }
        }
        for (int kind = 0; kind < 3; ++kind)
            for (size_t s = 0; s < npcRag_[kind].size(); ++s) {
                Ragdoll& r = npcRag_[kind][s];
                if (!r.active || (kab && kind != kNpcCreature)) continue;  // (his own grab takes Smarties and Vimps)
                float d = 0;
                const int k = r.nearest(p, d);
                nearestAny = std::min(nearestAny, d);
                if (k >= 0 && d < best) { best = d; part = k; pick = HeldBody{kind, int(s), r.uid, h}; }
            }
        if (part < 0) {
            if (trace) LOGI("grab trace: hand %d at (%.2f %.2f %.2f) - no body within reach %.2f m (nearest %.2f m)", h, p.x, p.y, p.z, reach, nearestAny);
            continue;
        }
        heldBody_ = pick;
        heldBody_.prev = c.gripPose.position;  // (tracking space: handVelocity)
        heldBody_.vel = Vec3{0, 0, 0};
        if (Ragdoll* r = bodyRagdoll(heldBody_)) {
            heldBody_.from = r->particle(size_t(part));
            r->pinParticle(part, heldBody_.from);
            r->posedAsleep = false;
        }
        haptics_.push_back({h, pick.kind == kReaperBody ? 0.7f : 0.45f, 0.1f});
        LOGI("Took hold of a %s body (%u) by part %d, %.2f m from the hand",
             pick.kind == kReaperBody ? "Reaper" : (pick.kind == kNpcCreature ? "creature" : (pick.kind == kNpcVillager ? "Smartie" : "Vimp")), pick.uid, part, best);
    }
    if (heldBody_.kind < 0) return;
    // Holding: the part follows the hand; the rest hangs and drags.
    HeldBody& b = heldBody_;
    Ragdoll* r = bodyRagdoll(heldBody_);
    const ControllerState& c = in.hand[b.hand];
    if (!r || !can || !c.gripPoseValid) {
        if (r) r->unpin();
        b = HeldBody{};
        return;
    }
    const Vec3 p = handAt(c);
    if (dt > 0) {
        const Vec3 hv = handVelocity(c.gripPose.position, b.prev, dt);
        b.vel = b.vel * 0.5f + hv * 0.5f;
        b.recent[b.nRecent++ % 12] = hv;
    }
    b.t += dt;
    // (the part eases into the hand over 0.15 s rather than jumping the reach)
    const float ease = std::min(1.0f, b.t / 0.15f);
    r->movePin(b.from + (p - b.from) * ease);
    r->posedAsleep = false;
    // The body's owner follows it (where it lies now: Cleaners, culling, the corpse's own logic).
    const Vec3 ctr = r->centre();
    if (b.kind == kReaperBody) {
        for (auto& e : enemies_.enemies)
            if (e.uid == b.uid) { e.pos.x = ctr.x; e.pos.z = ctr.z; }
    } else if (b.kind == kNpcVillager) {
        for (auto& v : villagers_.villagers)
            if (v.uid == b.uid) { v.pos.x = ctr.x; v.pos.z = ctr.z; }
    } else if (b.kind == kNpcVimp) {
        for (auto& v : vimps_.vimps)
            if (v.uid == b.uid) { v.pos.x = ctr.x; v.pos.z = ctr.z; }
    } else {
        for (auto& cr : creatures_.creatures)
            if (cr.uid == b.uid) { cr.pos.x = ctr.x; cr.pos.z = ctr.z; }
    }
    // Its weight in the hand: a steady pull, more for a Reaper, more when it swings.
    if (frand() < dt * 6.0f) haptics_.push_back({b.hand, (b.kind == kReaperBody ? 0.25f : 0.12f) + std::min(0.4f, 0.05f * length(b.vel)), 0.03f});
    if (trace && int(b.t / dt) % 18 == 0) LOGI("grab trace: hand (%.2f %.2f %.2f) %.1f m/s, body centre (%.2f %.2f %.2f)", p.x, p.y, p.z, length(b.vel), ctr.x, ctr.y, ctr.z);
    // Kabuto brings it to his mouth: he eats it.
    if (kab && b.kind == kReaperBody && in.headValid && kb_.eatT < 0 &&
        length(p - (rig_.toWorld(in.head).position - Vec3{0, 0.06f * H, 0})) < 0.22f * H) {
        for (size_t i = 0; i < enemies_.enemies.size(); ++i)
            if (enemies_.enemies[i].uid == b.uid) {
                r->unpin();
                LOGI("Kabuto: a body brought to his mouth (Reaper %u)", b.uid);
                kabutoEat(int(i));
                haptics_.push_back({b.hand, 0.8f, 0.2f});
                break;
            }
        b = HeldBody{};
        return;
    }
    if (c.squeeze < 0.35f) {
        r->unpin();
        // Let go: the part leaves at the hand's speed; a throw takes the body with it (a Smartie flies
        // further than a Reaper).
        // (the throw: the fastest the hand went in the last ~0.17 s - a hand opens as the arm slows)
        Vec3 thr = b.vel;
        for (int k = 0; k < std::min(b.nRecent, 12); ++k)
            if (length(b.recent[k]) > length(thr)) thr = b.recent[k];
        const float sp = length(thr);
        if (sp > 2.0f * (kab ? std::max(1.0f, gameScale()) : 1.0f)) {
            const float heavy = kab ? 0.9f : (b.kind == kReaperBody ? 0.55f : (b.kind == kNpcCreature ? 0.6f : 0.85f));  // (a doll to him)
            Vec3 kick = thr * heavy;
            if (length(kick) > 40.0f) kick = kick * (40.0f / length(kick));
            r->kick(kick);
            LOGI("Threw the body (%u) at %.1f m/s", b.uid, sp);
            thrownBody_ = b;
            thrownBody_.t = 0;
            thrownBody_.nHit = 0;
            thrownBody_.prev = r->centre();
        } else {
            LOGI("Let go of the body (%u)", b.uid);
        }
        b = HeldBody{};
    }
}

}  // namespace gvr
