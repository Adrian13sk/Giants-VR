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

Ragdoll* GameApp::heldRagdoll() {
    const HeldBody& b = heldBody_;
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

void GameApp::updateBodyGrab(float dt, const InputFrame& in) {
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
        if (Ragdoll* r = heldRagdoll()) {
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
    Ragdoll* r = heldRagdoll();
    const ControllerState& c = in.hand[b.hand];
    if (!r || !can || !c.gripPoseValid) {
        if (r) r->unpin();
        b = HeldBody{};
        return;
    }
    const Vec3 p = rig_.toWorld(c.gripPose).position;
    if (dt > 0) {
        b.vel = b.vel * 0.5f + (p - b.prev) * (0.5f / dt);
        b.recent[b.nRecent++ % 8] = (p - b.prev) * (1.0f / dt);
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
        // (the throw: the fastest the hand went in the last ~0.1 s - a hand opens as the arm slows)
        Vec3 thr = b.vel;
        for (int k = 0; k < std::min(b.nRecent, 8); ++k)
            if (length(b.recent[k]) > length(thr)) thr = b.recent[k];
        const float sp = length(thr);
        if (sp > 2.0f) {
            const float heavy = b.kind == kReaperBody ? 0.55f : (b.kind == kNpcCreature ? 0.6f : 0.85f);
            r->kick(thr * heavy);
            LOGI("Threw the body (%u) at %.1f m/s", b.uid, sp);
        } else {
            LOGI("Let go of the body (%u)", b.uid);
        }
        b = HeldBody{};
    }
}

}  // namespace gvr
