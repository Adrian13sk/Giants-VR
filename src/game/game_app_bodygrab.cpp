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
        const bool killed = enemies_.damage(int(i), 6.0f + 4.0f * sp);
        r->kick(v * -0.4f);  // (the body stops against him)
        sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", c, std::min(1.0f, 0.4f + sp * 0.08f), 1.0f);
        LOGI("The thrown body hits Reaper %u at %.1f m/s%s", e.uid, sp, killed ? " - he's dead" : "");
    }
}

void GameApp::updateBodyGrab(float dt, const InputFrame& in) {
    updateThrownBody(dt);
    const bool can = playMode_ && firstPerson_ && !dying_ && hero_ != Hero::Kabuto && !cutsceneActive() && !riding_;
    static const bool trace = std::getenv("GVR_GRAB_TRACE") != nullptr;
    // Taking hold: a grip squeezed (its edge) with the hand on a body, the other hand not holding one.
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        const bool g = c.squeeze > 0.7f;
        const bool edge = g && !bodyGripWas_[h];
        bodyGripWas_[h] = g;
        if (!can || !edge || heldBody_.kind >= 0 || !c.gripPoseValid || (heldVillager_ != 0 && heldVillagerHand_ == h)) continue;
        const Vec3 p = rig_.toWorld(c.gripPose).position;
        float best = kReach, nearestAny = 1e30f;
        HeldBody pick;
        int part = -1;
        for (auto& e : enemies_.enemies) {
            if (e.state != EnemyState::Dead || e.gibbed || e.posted || e.uid == impaledUid_) continue;
            if (length(e.pos - p) > 3.0f) continue;
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
                if (!r.active) continue;
                float d = 0;
                const int k = r.nearest(p, d);
                nearestAny = std::min(nearestAny, d);
                if (k >= 0 && d < best) { best = d; part = k; pick = HeldBody{kind, int(s), r.uid, h}; }
            }
        if (part < 0) {
            if (trace) LOGI("grab trace: hand %d at (%.2f %.2f %.2f) - no body within reach (nearest %.2f m)", h, p.x, p.y, p.z, nearestAny);
            continue;
        }
        heldBody_ = pick;
        heldBody_.prev = p;
        heldBody_.vel = Vec3{0, 0, 0};
        if (Ragdoll* r = bodyRagdoll(heldBody_)) {
            r->pinParticle(part, p);
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
    const Vec3 p = rig_.toWorld(c.gripPose).position;
    if (dt > 0) {
        b.vel = b.vel * 0.5f + (p - b.prev) * (0.5f / dt);
        b.recent[b.nRecent++ % 12] = (p - b.prev) * (1.0f / dt);
    }
    b.prev = p;
    b.t += dt;
    r->movePin(p);
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
    if (c.squeeze < 0.35f) {
        r->unpin();
        // Let go: the part leaves at the hand's speed; a throw takes the body with it (a Smartie flies
        // further than a Reaper).
        // (the throw: the fastest the hand went in the last ~0.17 s - a hand opens as the arm slows)
        Vec3 thr = b.vel;
        for (int k = 0; k < std::min(b.nRecent, 12); ++k)
            if (length(b.recent[k]) > length(thr)) thr = b.recent[k];
        const float sp = length(thr);
        if (sp > 2.0f) {
            const float heavy = b.kind == kReaperBody ? 0.55f : (b.kind == kNpcCreature ? 0.6f : 0.85f);
            r->kick(thr * heavy);
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
