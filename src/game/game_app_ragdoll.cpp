// Rework (user: "Try to implement ragdolls somehow"): dead Reapers fall as ragdolls (game/ragdoll.h).
// A body blown up, flung or killed in the air goes limp at once with the push it got; one shot dead
// plays the first moments of its death clip (the reaction) and then crumples with the shot's push.
// A body lying still sleeps (no more posing); a later blast wakes it. Delphi's sword pins one
// (game_app_delphi.cpp). Our own design.
#include <cmath>
#include <cstdlib>

#include "core/log.h"
#include "game/creature.h"
#include "scene/skinned_model.h"
#include "game/game_app.h"
#include "game/villager.h"
#include "game/vimp.h"
#include "scene/model_cache.h"

namespace gvr {

namespace {
// Giants model space (game units, Z-up) -> runtime (metres, Y-up): runtime = s * (-x, z, y).
Mat4 giantsBasis() {
    Mat4 c = Mat4::identity();
    const float s = gGameUnitsToMeters;
    c.m[0] = -s; c.m[5] = 0; c.m[6] = s; c.m[9] = s; c.m[10] = 0;
    return c;
}
}  // namespace

Mat4 GameApp::ragdollToWorld(const Enemy& e) const {
    // Model space (game units, Z-up) -> runtime world: the slot's render transform, plus the sink
    // (a sinking corpse sinks as a whole).
    const float lift = e.slot >= 0 && size_t(e.slot) < slotLift_.size() ? slotLift_[size_t(e.slot)] : 0.0f;
    return Mat4::translation(e.pos + Vec3{0, lift + e.sink, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, e.yaw)) * giantsBasis();
}

Ragdoll* GameApp::ragdollOf(const Enemy& e) {
    if (e.slot < 0 || size_t(e.slot) >= ragdolls_.size()) return nullptr;
    Ragdoll& r = ragdolls_[size_t(e.slot)];
    return (r.active || r.pending) && r.uid == e.uid ? &r : nullptr;
}

Ragdoll* GameApp::startRagdoll(Enemy& e, const Vec3& vel, const Vec3& spin) {
    if (ragdolls_.size() != enemyPool_.size()) ragdolls_.resize(enemyPool_.size());
    static const bool off = std::getenv("GVR_NO_RAGDOLL") != nullptr;
    if (off || e.state != EnemyState::Dead || e.posted || e.slot < 0 || size_t(e.slot) >= ragdolls_.size()) return nullptr;
    Ragdoll& r = ragdolls_[size_t(e.slot)];
    r.stop();
    r.pending = true;
    r.posedAsleep = false;
    r.uid = e.uid;
    r.clip = int(e.clip);
    r.startVel = vel;
    r.startSpin = spin;
    e.ragChecked = e.ragdoll = true;
    e.vel = e.kick = Vec3{0, 0, 0};
    return &r;
}

void GameApp::updateRagdolls(float dt) {
    if (ragdolls_.size() != enemyPool_.size()) ragdolls_.resize(enemyPool_.size());
    static const bool off = std::getenv("GVR_NO_RAGDOLL") != nullptr;
    static const bool trace = std::getenv("GVR_RAGDOLL_TRACE") != nullptr;
    for (auto& e : enemies_.enemies) {
        if (e.state != EnemyState::Dead) {
            e.kick = e.kick * std::max(0.0f, 1.0f - 6.0f * dt);
            e.ragdoll = false;
            continue;
        }
        if (off || e.posted || e.slot < 0 || size_t(e.slot) >= ragdolls_.size() || e.gibbed) continue;
        Ragdoll* rd = ragdollOf(e);
        if (!rd) {
            e.ragdoll = false;
            if (e.ragChecked || e.uid == impaledUid_) continue;
            const float k = length(e.kick);
            const bool now = k > 4.0f || e.clip == EnemyClip::Blown || e.clip == EnemyClip::FallLand;
            if (!now && e.stateTime < 0.35f) continue;  // the reaction first
            Vec3 v = e.kick * 0.7f + Vec3{e.vel.x, 0, e.vel.z} * 0.5f;
            Vec3 h{v.x, 0, v.z};
            float hl = length(h);
            if (hl > 7.0f) { h = h * (7.0f / hl); hl = 7.0f; }  // (thrown a few metres, not across the map)
            v = Vec3{h.x, std::min(6.0f, v.y), h.z};
            if (e.clip == EnemyClip::Blown) v.y = std::max(v.y, std::min(6.0f, 2.5f + 0.25f * k));
            startRagdoll(e, v, hl > 0.1f ? cross(Vec3{0, 1, 0}, h * (1.0f / hl)) * std::min(4.0f, 0.8f + 0.35f * hl) : Vec3{0, 0, 0});
            if (trace) LOGI("ragdoll: Reaper %u goes limp (push %.1f m/s%s)", e.uid, k, now ? ", at once" : "");
            continue;
        }
        e.ragdoll = true;
        e.vel = Vec3{0, 0, 0};
        if (rd->active && length(e.kick) > 0.5f) {
            rd->kick((e.kick + Vec3{0, 0.4f * length(e.kick), 0}) * 0.6f);
            rd->posedAsleep = false;
        }
        e.kick = Vec3{0, 0, 0};
        if (rd->active && !rd->asleep) {
            rd->step(dt, enemies_.groundHeight);
            const Vec3 c = rd->centre();
            e.pos.x = c.x;
            e.pos.z = c.z;
            if (rd->asleep && trace) LOGI("ragdoll: Reaper %u lies still (%.1f s) at %.1f %.1f %.1f, %.1f m from the player", e.uid, rd->age, c.x, rd->lowest(), c.z, length(c - player_.position));
        }
    }
}

// --- The other NPCs (rework): Rippers and other creatures, Smarties, Vimps -------------------
// Each model instance (a character model, a villager or vimp slot) has its ragdoll; the model's pose
// modifier starts it from the pose the body died in and applies it. A body killed by a blast goes
// limp at once with the blast's push (explode() records each blast); one killed otherwise plays the
// first moments of its death and crumples, pushed away from the player (shots come from him).

bool GameApp::npcToWorld(int kind, int slot, Mat4& out, uint32_t& uid) const {
    const Quat* q = nullptr;
    (void)q;
    if (kind == kNpcCreature) {
        if (slot < 0 || size_t(slot) >= chars_.size()) return false;
        for (const auto& c : creatures_.creatures)
            if (c.slot == slot) {
                uid = c.uid;
                out = chars_[size_t(slot)].xf() * giantsBasis();
                return true;
            }
        return false;
    }
    if (kind == kNpcVillager) {
        for (const auto& v : villagers_.villagers)
            if (v.slot == slot && size_t(slot) < villagerLift_.size()) {
                uid = v.uid;
                out = Mat4::translation(v.pos + Vec3{0, villagerLift_[size_t(slot)], 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, v.yaw)) *
                      giantsBasis();
                return true;
            }
        return false;
    }
    for (const auto& v : vimps_.vimps)
        if (v.slot == slot) {
            uid = v.uid;
            out = Mat4::translation(v.pos + Vec3{0, vimpLift_ * v.growth, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, v.yaw)) *
                  Mat4::scale(Vec3{v.growth, v.growth, v.growth}) * giantsBasis();
            return true;
        }
    return false;
}

bool GameApp::npcRagdolled(int kind, int slot) const {
    if (kind < 0 || kind > 2 || slot < 0 || size_t(slot) >= npcRag_[kind].size()) return false;
    const Ragdoll& r = npcRag_[kind][size_t(slot)];
    return r.active || r.pending;
}

Vec3 GameApp::npcKick(const Vec3& at, bool& blast) const {
    blast = false;
    for (const auto& b : recentBlasts_) {
        const Vec3 d = at - b.pos;
        const float L = length(d), R = std::max(2.0f, b.radius * 1.6f);
        if (L >= R) continue;
        blast = true;
        const float k = 1.0f - L / R;
        const Vec3 h = L > 0.05f ? Vec3{d.x / L, 0, d.z / L} : Vec3{0, 0, 0};
        return h * (2.0f + 6.0f * k) + Vec3{0, 2.0f + 4.0f * k, 0};
    }
    Vec3 d = at - player_.position;
    d.y = 0;
    const float L = length(d);
    return (L > 0.05f ? d * (1.0f / L) : Vec3{0, 0, 0}) * 1.6f + Vec3{0, 0.4f, 0};
}

void GameApp::updateNpcRagdolls(float dt) {
    static const bool off = std::getenv("GVR_NO_RAGDOLL") != nullptr;
    static const bool trace = std::getenv("GVR_RAGDOLL_TRACE") != nullptr;
    // A new world (or new model sets): one ragdoll per model, hooked into its pose.
    const size_t want[3] = {chars_.size(), villagerModels_.size(), vimpModels_.size()};
    if (npcRagWorld_ != assetWorld_.get() || npcRag_[0].size() != want[0] || npcRag_[1].size() != want[1] || npcRag_[2].size() != want[2]) {
        npcRagWorld_ = assetWorld_.get();
        for (int kind = 0; kind < 3; ++kind) {
            npcRag_[kind].assign(want[kind], Ragdoll{});
            for (size_t k = 0; k < want[kind]; ++k) {
                SkinnedModel* m = kind == kNpcCreature ? chars_[k].model.get() : (kind == kNpcVillager ? villagerModels_[k].get() : vimpModels_[k].get());
                if (!m) continue;
                const int slot = int(k);
                m->setPoseModifier([this, kind, slot](const AnmAnimation& clip, const std::vector<std::vector<int>>&, std::vector<Mat4>& world) {
                    if (size_t(slot) >= npcRag_[kind].size()) return;
                    Ragdoll& r = npcRag_[kind][size_t(slot)];
                    if (!r.active && !r.pending) return;
                    Mat4 tw;
                    uint32_t uid = 0;
                    if (!npcToWorld(kind, slot, tw, uid) || uid != r.uid) return;
                    if (r.pending) {
                        std::vector<int> parent(clip.nodes.size(), -1);
                        for (size_t n = 0; n < clip.nodes.size(); ++n) parent[n] = clip.nodes[n].parent;
                        r.start(world, parent, tw, r.startVel, r.startSpin, 0.35f);
                        r.pending = false;
                    }
                    r.apply(world, tw);
                });
            }
        }
    }
    recentBlasts_.erase(std::remove_if(recentBlasts_.begin(), recentBlasts_.end(), [this](const RecentBlast& b) { return time_ - b.t > 0.25; }),
                        recentBlasts_.end());
    if (off) return;
    // One body: dead (and with a body to show) -> starts once (after its first moments unless blasted),
    // then steps until it lies still; alive again (a reborn Vimp, a reused model) -> stops.
    auto body = [&](int kind, int slot, uint32_t uid, bool dead, float sinceDeath, const Vec3& at, float heavy) {
        if (slot < 0 || size_t(slot) >= npcRag_[kind].size()) return;
        Ragdoll& r = npcRag_[kind][size_t(slot)];
        const bool mine = (r.active || r.pending) && r.uid == uid;
        if (!dead) {
            if (mine) r.stop(), r.pending = false;
            return;
        }
        if (!mine) {
            if (r.uid == uid && r.age > 0) return;  // (already had its ragdoll: it lies as it fell)
            bool blast = false;
            const Vec3 k = npcKick(at, blast);
            if (!blast && sinceDeath < 0.3f) return;  // the reaction first
            if (sinceDeath > 2.0f && !blast) { r.uid = uid; r.age = 1; return; }  // (died long ago, before this: as it lies)
            r.stop();
            r.pending = true;
            r.uid = uid;
            r.startVel = k * heavy;
            const Vec3 h{k.x, 0, k.z};
            const float hl = length(h);
            r.startSpin = hl > 0.1f ? cross(Vec3{0, 1, 0}, h * (1.0f / hl)) * std::min(4.0f, 1.0f + 0.4f * hl) * heavy : Vec3{0, 0, 0};
            if (trace) LOGI("ragdoll: %s %u goes limp (%s, push %.1f m/s)", kind == kNpcCreature ? "creature" : (kind == kNpcVillager ? "Smartie" : "Vimp"), uid,
                            blast ? "blast" : "killed", length(r.startVel));
            return;
        }
        if (r.active && !r.asleep) {
            r.step(dt, enemies_.groundHeight);
            if (r.asleep && trace) LOGI("ragdoll: npc %u lies still (%.1f s)", uid, r.age);
        }
    };
    for (const auto& c : creatures_.creatures)
        if (!c.exploded && !CreatureDirector::underground(c))
            body(kNpcCreature, c.slot, c.uid, c.state == CreatureState::Dead, c.stateTime, c.pos, 1.0f);
    for (const auto& v : villagers_.villagers) body(kNpcVillager, v.slot, v.uid, v.state == VillagerState::Dead, v.stateTime, v.pos, 1.1f);
    for (const auto& v : vimps_.vimps) body(kNpcVimp, v.slot, v.uid, v.state == VimpState::Dead && !v.eaten, v.stateTime, v.pos, 0.6f);
}

}  // namespace gvr
