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
#include "scene/asset_world.h"
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

void GameApp::ragdollSplash(Ragdoll& r, float size) {
    // A body (or a limb of it) going into the water fast: a splash where it went in.
    if (r.splash > 2.0f && length(r.splashAt - player_.position) < 80.0f) {
        waterSplash(r.splashAt, size * std::min(1.6f, 0.35f + r.splash * 0.12f), r.splash > 3.5f ? 2 : 1);
        static const bool trace = std::getenv("GVR_RAGDOLL_TRACE") != nullptr;
        if (trace) LOGI("ragdoll: a body splashes into the water at %.1f m/s", r.splash);
    }
    r.splash = 0;
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
            // A blast right at him tears a limb off now and then (the one nearest the burst).
            static const bool always = std::getenv("GVR_DISMEMBER_ALWAYS") != nullptr;  // (test hook)
            if (std::getenv("GVR_DISMEMBER_TRACE"))
                for (const auto& b : recentBlasts_) LOGI("dismember trace: Reaper %u clip %d, a blast %.2f m away (radius %.1f)", e.uid, int(e.clip), length(b.pos - e.pos), b.radius);
            if (e.clip == EnemyClip::Blown && !e.gibbed)
                for (const auto& b : recentBlasts_)
                    if (length(b.pos - e.pos) < std::max(2.0f, b.radius * 0.6f) && (always || frand() < 0.6f)) {
                        const Vec3 mid = e.pos + Vec3{0, 1.0f, 0};
                        // The limb nearest the burst (pulled toward the body's middle, so a burst at
                        // the feet takes a leg and one at the chest an arm).
                        dismember(e, b.pos + (mid - b.pos) * 0.35f, mid - b.pos);
                        break;
                    }
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
            rd->waterLevel = player_.seaLevel;
            rd->step(dt, enemies_.groundHeight);
            ragdollSplash(*rd, 1.0f);
            // The body hits the ground: a thud with the force of it (at most every 0.3 s).
            if (rd->impact > 2.5f && time_ - rd->lastThud > 0.3) {
                rd->lastThud = time_;
                const float loud = std::min(1.0f, 0.3f + rd->impact / 9.0f);
                sfx(rd->impact > 6.0f ? (frand() < 0.5f ? "sr_fall1" : "sr_fall2") : (frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2"), rd->centre(), loud, 0.9f + 0.2f * frand());
            }
            rd->impact = 0;
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
            r.waterLevel = player_.seaLevel;
            r.step(dt, enemies_.groundHeight);
            ragdollSplash(r, 0.6f * heavy);
            if (r.impact > 2.5f && time_ - r.lastThud > 0.3) {
                r.lastThud = time_;
                sfx(frand() < 0.5f ? "ex_bdys1" : "ex_bdys2", r.centre(), std::min(1.0f, 0.25f + r.impact / 10.0f) * heavy, 1.0f + 0.2f * frand());
            }
            r.impact = 0;
            if (r.asleep && trace) LOGI("ragdoll: npc %u lies still (%.1f s)", uid, r.age);
        }
    };
    for (const auto& c : creatures_.creatures)
        if (!c.exploded && !CreatureDirector::underground(c))
            body(kNpcCreature, c.slot, c.uid, c.state == CreatureState::Dead, c.stateTime, c.pos, 1.0f);
    for (const auto& v : villagers_.villagers) body(kNpcVillager, v.slot, v.uid, v.state == VillagerState::Dead, v.stateTime, v.pos, 1.1f);
    for (const auto& v : vimps_.vimps) body(kNpcVimp, v.slot, v.uid, v.state == VimpState::Dead && !v.eaten, v.stateTime, v.pos, 0.6f);
}

// --- Dismemberment (rework: "Make Delphi's sword stick in enemies, perhaps dismemberment?") ----
std::string GameApp::nearestLimb(const Enemy& e, const Vec3& at, float& bd) {
    bd = 1e30f;
    if (e.slot < 0 || size_t(e.slot) >= enemyPool_.size()) return {};
    SkinnedModel& m = *enemyPool_[size_t(e.slot)];
    const float lift = size_t(e.slot) < slotLift_.size() ? slotLift_[size_t(e.slot)] : 0.0f;
    const Quat yq = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
    const Vec3 origin = e.pos + Vec3{0, lift + e.sink, 0};
    // The blow in model space; the limb whose vertices lie nearest it.
    const Quat inv{-yq.x, -yq.y, -yq.z, yq.w};
    const Vec3 local = inv.rotate(at - origin);
    // Each limb by the first name the skin has (rpman_l0: l_arm, l_thigh; others: "arm l", "leg l").
    static const std::vector<std::vector<const char*>> kLimbs = {
        {"head"}, {"l_arm", "arm l", "l-arm"}, {"r_arm", "arm r"}, {"l_thigh", "leg l", "l_thigh01"}, {"r_thigh", "leg r"}};
    static const bool tr = std::getenv("GVR_DISMEMBER_TRACE") != nullptr;
    std::string best;
    Vec3 headC;
    const bool haveHead = m.boneCentre("head", headC);
    for (const auto& names : kLimbs) {
        Vec3 c;
        const char* b = nullptr;
        for (const char* nm : names)
            if (m.boneCentre(nm, c)) { b = nm; break; }
        if (!b) continue;
        // The head only for a blow at head height (its centre is nearer a chest hit than the arms are).
        if (haveHead && std::string(b) == "head" && local.y < headC.y - 0.22f) continue;
        const float d = length(c - local);
        if (tr) LOGI("dismember trace: %s %.2f m from the blow", b, d);
        if (d < bd) { bd = d; best = b; }
    }
    return best;
}

bool GameApp::reaperGunCentre(const Enemy& e, Vec3& out) const {
    if (!e.weaponOn || e.weapon < 0 || e.weapon > 1 || !rpWeapon_[e.weapon].model) return false;
    const RenderModel& m = *rpWeapon_[e.weapon].model;
    const Vec3 c = (m.boundsMin + m.boundsMax) * 0.5f;
    const Vec4 w = e.weaponXf * Vec4{c.x, c.y, c.z, 1.0f};
    out = Vec3{w.x, w.y, w.z};
    return true;
}

void GameApp::dropReaperGun(Enemy& e, const Vec3& vel) {
    if (!e.weaponOn || e.weapon < 0 || e.weapon > 1 || !rpWeapon_[e.weapon].model) return;
    Gib gun;
    gun.model = rpWeapon_[e.weapon].model;
    // It falls and spins about its middle (the model's origin is far from the gun - near his feet -
    // so a gib at the origin "landed" at once, silently, and turned about a point on the ground).
    const RenderModel& m = *rpWeapon_[e.weapon].model;
    const Vec3 c = (m.boundsMin + m.boundsMax) * 0.5f;
    reaperGunCentre(e, gun.pos);
    gun.base = e.weaponXf;
    gun.base.m[12] = gun.base.m[13] = gun.base.m[14] = 0;
    gun.base = gun.base * Mat4::translation(c * -1.0f);
    gun.vel = vel;
    gun.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
    gun.spin = 2.0f + 4.0f * frand();
    gun.scale = 1.0f;
    gun.owner = e.uid;
    gun.rpKind = e.weapon;
    gun.ammo = e.weapon == 1 ? 3 : 12;  // (what is left in it: a few rockets, a dozen bolts)
    gibs_.push_back(gun);
    e.weaponOn = false;
    LOGI("Reaper %u drops his gun at (%.1f %.1f %.1f)", e.uid, gun.pos.x, gun.pos.y, gun.pos.z);
}

bool GameApp::dismember(Enemy& e, const Vec3& at, const Vec3& dir) {
    float bd = 0;
    std::string best = nearestLimb(e, at, bd);
    if (e.slot < 0 || size_t(e.slot) >= enemyPool_.size()) return false;
    SkinnedModel& m = *enemyPool_[size_t(e.slot)];
    const float lift = size_t(e.slot) < slotLift_.size() ? slotLift_[size_t(e.slot)] : 0.0f;
    const Quat yq = Quat::fromAxisAngle({0, 1, 0}, e.yaw);
    const Vec3 origin = e.pos + Vec3{0, lift + e.sink, 0};
    if (best.empty() || bd > 1.2f) return false;
    if (const char* force = std::getenv("GVR_DISMEMBER_LIMB")) best = force;  // (test hook: that limb)
    std::vector<SkinnedModel::Piece> pieces;
    Vec3 joint;
    if (!m.sever(renderer_, best, pieces, joint)) return false;
    Renderer* r = &renderer_;
    std::shared_ptr<RenderModel> own(new RenderModel, [r](RenderModel* rm) {
        for (const auto& p : rm->parts) r->destroyMesh(p.mesh);
        delete rm;
    });
    own->name = "limb";
    for (const auto& pc : pieces) {
        RenderModel::Part part;
        part.mesh = pc.mesh;
        part.texture = pc.texture;
        part.blend = pc.blend;
        part.lit = pc.lit;
        own->parts.push_back(part);
    }
    Gib g;
    g.model = own.get();
    g.own = own;
    g.base = Mat4::rotation(yq);
    g.pos = origin + yq.rotate(joint);
    Vec3 h{dir.x, 0, dir.z};
    if (length(h) > 1e-3f) h = normalize(h);
    g.vel = h * (2.5f + 2.0f * frand()) + Vec3{(frand() - 0.5f) * 1.5f, 2.5f + 2.0f * frand(), (frand() - 0.5f) * 1.5f};
    g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
    g.spin = 3.0f + 5.0f * frand();
    g.scale = 1.0f;
    gibs_.push_back(g);
    // The gun hand off: the gun goes with it (thrown along, from where it was in the hand).
    if (best == "r_arm" || best == "arm r") dropReaperGun(e, g.vel * 0.8f + Vec3{0, 1.0f, 0});
    if (best == "r_arm" || best == "arm r") e.gunArmOff = true;
    sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", g.pos, 1.0f, 0.8f + 0.15f * frand());  // the cut
    // The stump and the piece bleed.
    blood_.burst(g.pos, h + Vec3{0, 1.2f, 0}, 30, 4.5f, 0.08f);
    blood_.pool(e.pos, 1.0f);
    LOGI("dismember: Reaper %u loses his %s", e.uid, best.c_str());
    return true;
}

void GameApp::spawnBones(const Vec3& at, int n, const Vec3& push, float speed) {
    if (!assetWorld_) return;
    if (!boneModels_[0] && assetWorld_->models()) {
        ModelCache& mc = *assetWorld_->models();
        boneModels_[0] = mc.get("Bone_femur.gbs", "", true);
        boneModels_[1] = mc.get("Bone_rib.gbs", "", true);
        boneModels_[2] = mc.get("Bone_spine.gbs", "", true);
        if (!boneModels_[0]) LOGW("Bones: models unavailable");
    }
    int made = 0;
    for (int k = 0; k < n; ++k) {
        const RenderModel* m = boneModels_[k % 3];
        if (!m) continue;
        Gib g;
        g.model = m;
        g.pos = at;
        g.vel = push * speed + Vec3{(frand() - 0.5f) * speed, 0.5f * speed + frand() * speed, (frand() - 0.5f) * speed};
        g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
        g.angle = frand() * 6.28f;
        g.spin = 3.0f + 5.0f * frand();
        // (the art is giant-sized - a 4 m femur, a 9 m rib, for skeleton scenery: shrunk to a body's bones)
        const Vec3 ext = m->boundsMax - m->boundsMin;
        const float big = std::max(ext.x, std::max(ext.y, ext.z));
        const float want = k % 3 == 0 ? 0.45f : (k % 3 == 1 ? 0.35f : 0.3f);
        g.scale = big > 1e-3f ? want / big : 1.0f;
        gibs_.push_back(g);
        ++made;
    }
    if (made) LOGI("Bones: %d left at (%.1f %.1f %.1f)", made, at.x, at.y, at.z);
}

}  // namespace gvr
