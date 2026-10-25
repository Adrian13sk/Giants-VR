// GameApp: Vimp herds, Vimp meat and the "Vimp meat proximity" mission trigger. Same class; see
// game_app.h ("Vimp herds") and vimp.h.
#include "game/game_app.h"
#include "game/player.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "audio/mixer.h"
#include "core/log.h"
#include "game/destruction.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
// Worlds with Vimps (ours: the original spawns them in native code). The Smartie lands of the
// Mecc story from chapter 2, and Kabuto's islands (he eats them).
bool vimpWorld(const std::string& world) {
    const std::string w = toLower(world);
    if (w.rfind("kstory", 0) == 0) return true;
    if (w == "rstory4" || w == "rstory2" || w == "rstory5") return true;  // the Reaper base missions: "Gather SMARTIES and VIMP SOULS"
    return w.rfind("story", 0) == 0 && w.size() == 6 && w[5] >= '2' && w[5] <= '5';
}
}  // namespace

void GameApp::initVimps(AssetWorld* w, ModelCache* mc) {
    vimps_ = VimpDirector{};
    vimpModels_.clear();
    meat_.clear();
    if (!w || !w->hasTerrain() || !mc) return;
    meatModel_[0] = mc->get("VimpFoodChunk1.gbs", "", true);
    meatModel_[1] = mc->get("VimpFoodChunk2.gbs", "", true);
    meatModel_[2] = mc->get("VimpFoodChunk3.gbs", "", true);
    meatIcon_ = mc->texture("st_vimpmeat");
    dropoffTex_ = mc->texture("vimp_dropoff");
    if (!vimpWorld(w->worldName()) || std::getenv("GVR_NO_VIMPS")) return;
    const Terrain& terr = w->terrain();
    vimps_.groundHeight = [w](float x, float z) {
        float y;
        return w->terrain().heightAt(x, z, y) ? y : 0.0f;
    };
    // Grazing land: above the beach, not steep.
    auto grass = [w](float x, float z) {
        float y, a, b;
        const Terrain& t = w->terrain();
        if (!t.heightAt(x, z, y) || y < 1.5f) return false;
        if (!t.heightAt(x + 3.0f, z, a) || !t.heightAt(x, z + 3.0f, b)) return false;
        return std::fabs(a - y) < 1.6f && std::fabs(b - y) < 1.6f;
    };
    vimps_.walkable = grass;
    // Herd sites: open grass away from villages, bases and the start (deterministic per world).
    std::vector<Vec3> avoid;
    for (const auto& o : w->worldObjects())
        if (o.name.rfind("smw_", 0) == 0 || o.name.rfind("sm_", 0) == 0 || o.name.rfind("guard", 0) == 0 || o.name.rfind("barracks", 0) == 0 ||
            o.name.rfind("bw_", 0) == 0)
            avoid.push_back(o.centre);
    const Vec3 spawn = w->spawnPoint();
    uint32_t seed = 1469598103u;
    for (char c : toLower(w->worldName())) seed = (seed ^ uint8_t(c)) * 16777619u;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1u << 24); };
    const Vec3 mn = terr.boundsMin(), mx = terr.boundsMax();
    std::vector<Vec3> sites;
    const int kHerds = 6;
    for (int tries = 0; tries < 4000 && int(sites.size()) < kHerds; ++tries) {
        const Vec3 p{mn.x + rnd() * (mx.x - mn.x), 0, mn.z + rnd() * (mx.z - mn.z)};
        if (!grass(p.x, p.z)) continue;
        int ok = 0;  // a patch of grass, not a ledge
        for (int k = 0; k < 8; ++k) ok += grass(p.x + 12.0f * std::cos(k * 0.785f), p.z + 12.0f * std::sin(k * 0.785f));
        if (ok < 7) continue;
        bool near = length(Vec3{p.x - spawn.x, 0, p.z - spawn.z}) < 50.0f;
        for (const Vec3& a : avoid) near |= length(Vec3{p.x - a.x, 0, p.z - a.z}) < 45.0f;
        for (const Vec3& s : sites) near |= length(Vec3{p.x - s.x, 0, p.z - s.z}) < 90.0f;
        if (!near) sites.push_back(p);
    }
    static const char* clips[int(VimpClip::Count)] = {"vp_graze.anm", "vp_walk.anm", "vp_gallop.anm", "vp_scared.anm",
                                                      "vp_hit.anm",   "vp_hit2.anm", "vp_idle.anm"};
    for (size_t h = 0; h < sites.size(); ++h) {
        Vec3 c = sites[h];
        c.y = vimps_.groundHeight(c.x, c.z);
        const int herd = vimps_.addHerd(c);
        const int n = 3 + int(rnd() * 3.0f);
        for (int k = 0; k < n; ++k) {
            Vec3 p = c + Vec3{(rnd() - 0.5f) * 14.0f, 0, (rnd() - 0.5f) * 14.0f};
            if (!grass(p.x, p.z)) p = c;
            p.y = vimps_.groundHeight(p.x, p.z);
            auto m = std::make_unique<SkinnedModel>();
            if (!m->load(w->fs(), *mc, renderer_, "vp_l1", clips[0])) { LOGW("Vimp model unavailable"); return; }
            for (int i = 1; i < int(VimpClip::Count); ++i) {
                const int id = m->addAnimation(w->fs(), clips[i]);
                if (vimpModels_.empty()) vimpClip_[i] = std::max(0, id);
            }
            if (vimpModels_.empty()) {
                Vec3 bmn, bmx;
                m->boundsAt(0, 0.0f, bmn, bmx);
                vimpLift_ = -bmn.y;
            }
            m->pose(renderer_, 0.0f);
            const int i = vimps_.add(herd, p, rnd() * 6.2832f);
            vimps_.vimps[size_t(i)].slot = int(vimpModels_.size());
            vimpModels_.push_back(std::move(m));
        }
        LOGI("Vimp herd %zu: %d at game (%.0f, %.0f)", h, n, -c.x / gGameUnitsToMeters, c.z / gGameUnitsToMeters);
    }
}

bool GameApp::vimpRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& index) const {
    bool hit = false;
    for (size_t i = 0; i < vimps_.vimps.size(); ++i) {
        const Vimp& v = vimps_.vimps[i];
        if (v.state == VimpState::Dead) continue;
        // A sphere round the body (about 3 m long, the back 3 m up).
        const float r = 1.5f;
        const Vec3 c = v.pos + Vec3{0, 2.0f, 0};
        const Vec3 oc = o - c;
        const float b = dot(oc, d), cc = dot(oc, oc) - r * r, disc = b * b - cc;
        if (disc < 0) continue;
        float tn = -b - std::sqrt(disc);
        if (tn < 0 && cc <= 0) tn = 0;  // (fired from inside it, point blank: a hit)
        if (tn < 0 || tn > maxD || (hit && tn >= t)) continue;
        t = tn;
        index = int(i);
        hit = true;
    }
    return hit;
}

void GameApp::hitVimp(int i, float amount, const Vec3& at, const Vec3& dir) {
    if (i < 0 || size_t(i) >= vimps_.vimps.size()) return;
    const Vec3 pos = vimps_.vimps[size_t(i)].pos;
    const bool died = vimps_.damage(i, amount, at - dir * 5.0f);
    bleed(at, dir, died, 1.4f);
    if (died) {
        sfx(frand() < 0.5f ? "vi_deat1" : "vi_deat2", pos, 1.0f);
        dropMeat(pos + Vec3{0, 1.6f, 0}, 3);
        blood_.pool(pos, 1.8f);
        LOGI("Vimp killed: %zu left", vimps_.aliveCount());
    } else {
        sfx(frand() < 0.5f ? "vi_injr1" : "vi_injr2", pos, 1.0f);
    }
}

void GameApp::dropMeat(const Vec3& at, int pieces) {
    for (int k = 0; k < pieces; ++k) {
        Meat m;
        m.pos = at;
        const float a = frand() * 6.2832f;
        m.vel = Vec3{std::cos(a) * (1.5f + 2.0f * frand()), 3.0f + 2.5f * frand(), std::sin(a) * (1.5f + 2.0f * frand())};
        m.yaw = frand() * 6.2832f;
        m.spin = (frand() - 0.5f) * 8.0f;
        m.model = int(frand() * 2.99f);
        meat_.push_back(m);
    }
}

void GameApp::updateVimps(float dt) {
    if (!vimps_.vimps.empty()) {
        std::vector<Vec3> threats, scares;
        if (playMode_ && !dying_) threats.push_back(player_.position);
        for (const auto& e : enemies_.enemies)
            if (e.state != EnemyState::Dead) threats.push_back(e.pos);
        for (const auto& b : blasts_)
            if (b.age < 0.05f) scares.push_back(b.pos);
        if (weapon_.shotsFired() != vimpShots_) {
            vimpShots_ = weapon_.shotsFired();
            scares.push_back(player_.position);
        }
        vimps_.update(dt, threats, scares);
        // Sounds now and then from a grazing herd nearby.
        if (frand() < dt * 0.15f) {
            const Vimp& v = vimps_.vimps[size_t(frand() * 0.999f * float(vimps_.vimps.size()))];
            if (v.state != VimpState::Dead && length(v.pos - player_.position) < 60.0f)
                sfx(v.state == VimpState::Flee ? "vi_squi1" : (frand() < 0.5f ? "vi_moo1" : "vi_moo2"), v.pos + Vec3{0, 2, 0}, 0.8f);
        }
        for (const auto& v : vimps_.vimps) {
            if (v.slot < 0 || length(v.pos - player_.position) > 120.0f) continue;
            const int clip = vimpClip_[int(v.clip)];
            float t = v.clipTime;
            if (v.state == VimpState::Dead || v.state == VimpState::Hit) t = std::min(t, vimpModels_[size_t(v.slot)]->animDuration(clip) - 0.01f);
            vimpModels_[size_t(v.slot)]->poseBlend(renderer_, clip, t, clip, t, 0.0f);
        }
    }
    // Meat: falls, bounces, lies; walked over, it's picked up (the giant eats it).
    meatFlash_ = std::max(0.0f, meatFlash_ - dt * 1.5f);
    meatHintT_ = std::max(0.0f, meatHintT_ - dt);
    const float reach = 1.6f + (player_.radius - 0.35f);
    for (auto& m : meat_) {
        m.t += dt;
        if (!m.rest) {
            m.vel.y -= 14.0f * dt;
            m.pos = m.pos + m.vel * dt;
            m.yaw += m.spin * dt;
            float g = 0;
            if (assetWorld_ && assetWorld_->terrain().heightAt(m.pos.x, m.pos.z, g) && m.pos.y < g + 0.15f) {
                m.pos.y = g + 0.15f;
                if (m.vel.y < -2.0f) { m.vel = Vec3{m.vel.x * 0.4f, -m.vel.y * 0.3f, m.vel.z * 0.4f}; m.spin *= 0.5f; }
                else m.rest = true;
            }
        }
        if (playMode_ && !dying_ && m.t > 0.5f && length(Vec3{m.pos.x - player_.position.x, 0, m.pos.z - player_.position.z}) < reach &&
            std::fabs(m.pos.y - player_.position.y) < 2.0f + player_.height) {
            m.t = -1e9f;
            if (hero_ == Hero::Kabuto) {
                playerHealth_ = std::min(100.0f, playerHealth_ + 5.0f);
                sfx2D(frand() < 0.5f ? "ka_vimp1" : "ka_vimp2", 0.6f);
            } else {
                ++vimpMeat_;
                meatFlash_ = 1.0f;
                sfx2D(frand() < 0.5f ? "pu_meat1" : "pu_meat2", 0.9f);
            }
            haptics_.push_back({rig_.moveHand(), 0.3f, 0.05f});
        }
    }
    meat_.erase(std::remove_if(meat_.begin(), meat_.end(), [](const Meat& m) { return m.t < -1e8f || m.t > 240.0f; }), meat_.end());
    // Mission triggers of kind 4: the meat brought to them (e.g. Timmy's father in 2a).
    if (playMode_ && !missionComplete_) {
        const Vec3 p = player_.position + Vec3{0, 0.9f, 0};
        for (size_t i = 0; i < objectives_.size(); ++i) {
            const Objective& o = objectives_[i];
            if (o.done || o.kind != 4) continue;
            const Vec3 c{std::max(o.boundsMin.x, std::min(o.boundsMax.x, p.x)), std::max(o.boundsMin.y, std::min(o.boundsMax.y, p.y)),
                         std::max(o.boundsMin.z, std::min(o.boundsMax.z, p.z))};
            if (length(c - p) > 3.0f + (player_.radius - 0.35f)) continue;
            if (vimpMeat_ >= kVimpMeatNeeded) {
                vimpMeat_ -= kVimpMeatNeeded;
                LOGI("Vimp meat delivered");
                completeObjective(int(i));
            } else if (meatHintT_ <= 0) {
                meatHintT_ = 6.0f;
                LOGI("Vimp meat: %d of %d", vimpMeat_, kVimpMeatNeeded);
                timmyLine_ = "VIMP MEAT: " + std::to_string(vimpMeat_) + " OF " + std::to_string(kVimpMeatNeeded) + " - HUNT SOME MORE VIMPS!";
                timmyLineT_ = 4.0f;
            }
        }
    }
}

void GameApp::renderVimps(bool shadow, const Vec3& eye, float range) {
    for (const auto& v : vimps_.vimps) {
        if (v.slot < 0 || length(v.pos - eye) > range) continue;
        const Mat4 xf = Mat4::translation(v.pos + Vec3{0, vimpLift_, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, v.yaw));
        if (shadow) vimpModels_[size_t(v.slot)]->renderShadow(renderer_, xf);
        else vimpModels_[size_t(v.slot)]->render(renderer_, xf);
    }
    if (shadow) return;
    for (const auto& m : meat_) {
        if (m.t < 0 || length(m.pos - eye) > range) continue;
        drawModel(meatModel_[m.model], Mat4::translation(m.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, m.yaw)), false);
    }
    // The drop-off point (the original's vimp_dropoff marker) over kind-4 triggers.
    if (dropoffTex_ && text_.ready())
        for (const auto& o : objectives_) {
            if (o.done || o.kind != 4) continue;
            const Vec3 c = (o.boundsMin + o.boundsMax) * 0.5f + Vec3{0, 3.5f + 0.25f * std::sin(float(time_) * 2.0f), 0};
            const float d = length(c - eye);
            if (d > 400.0f) continue;
            const float s = std::max(1.2f, d * 0.04f);
            text_.drawImage(Mat4::translation(c) * Mat4::rotation(headWorldRot_), s, s, dropoffTex_, {1, 1, 1, 0.9f});
        }
}

}  // namespace gvr

namespace gvr {

// Mission-target characters without their own AI yet (see CharacterInst::boss).
bool GameApp::bossRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& index) const {
    bool hit = false;
    for (size_t i = 0; i < chars_.size(); ++i) {
        const CharacterInst& ci = chars_[i];
        if (!ci.boss || ci.hp <= 0 || !ci.visible) continue;
        // Upright cylinder approximated by a sphere stack along the body.
        for (float f = 0.25f; f < 1.0f; f += 0.25f) {
            const Vec3 c = ci.pos + Vec3{0, ci.bossH * f, 0};
            const float r = ci.bossR;
            const Vec3 oc = o - c;
            const float b = dot(oc, d), cc = dot(oc, oc) - r * r, disc = b * b - cc;
            if (disc < 0) continue;
            float tn = -b - std::sqrt(disc);
            if (tn < 0 && cc <= 0) tn = 0;  // (fired from inside it, point blank: a hit)
            if (tn < 0 || tn > maxD || (hit && tn >= t)) continue;
            t = tn;
            index = int(i);
            hit = true;
        }
    }
    return hit;
}

namespace {
const char* const kBossHurt[5] = {"", "ka_mdht1", "of_dhit1", "ca_injr1", "so_injr1"};
const char* const kBossDie[5] = {"", "ka_deth1", "of_deth1", "ca_deat1", "so_deat1"};
}  // namespace

void GameApp::hitBoss(int i, float amount, const Vec3& at, const Vec3& dir) {
    if (i < 0 || size_t(i) >= chars_.size()) return;
    CharacterInst& ci = chars_[size_t(i)];
    if (!ci.boss || ci.hp <= 0) return;
    if (ci.bossKb && hero_ != Hero::Kabuto) {
        // "Fire at the WEAK SPOT on Kabuto's stomach ... the only way to damage him" (kstory5_5's
        // hint): the belly takes triple damage, the rest of his hide little (ours: no cover plate).
        const float h = (at.y - ci.pos.y) / std::max(1.0f, ci.bossH);
        amount *= (h > 0.35f && h < 0.62f) ? 3.0f : 0.25f;
    }
    ci.hp -= amount;
    static const bool bossTrace = std::getenv("GVR_KB_TRACE") != nullptr;
    if (bossTrace) LOGI("boss %d hit for %.0f: %.0f hp left", i, amount, ci.hp);
    bleed(at, dir, ci.hp <= 0, std::max(1.0f, ci.bossH / 2.0f));
    if (ci.bossKind > 0 && ci.hp > 0 && frand() < 0.2f) sfx(kBossHurt[ci.bossKind], at, 1.0f);
    if (ci.hp <= 0) {
        ci.dieT = 0;
        blood_.pool(ci.pos, std::max(1.5f, ci.bossR * 2.0f));
        sfx(ci.bossKind > 0 ? kBossDie[ci.bossKind] : "ex_bdyl1", at, 1.0f, ci.bossKind > 0 ? 1.0f : 0.8f);
        ci.bossClipT = 0;
        if (ci.objective >= 0) completeObjective(ci.objective);
        LOGI("Mission target killed (character %d)", i);
    }
}

namespace {
// Boss AI per kind (ours: the original's is native code). Clips: stand, move, attack 1, attack 2,
// roar, die. Attack 2 is Kabuto's stomp (a quake), the Offspring's bite and the Charger's charge.
struct BossSpec {
    const char* clips[6];
    float speed, reach;        // m/s approaching; reach as a fraction of its height (+ the player's radius)
    float hit1, hit2;          // damage
    float at1, at2;            // when in the clip the blow lands (fraction)
    const char* sAttack1; const char* sAttack2; const char* sRoar; const char* sHurt; const char* sDie;
};
const BossSpec kBoss[5] = {
    {},
    {{"kb_stand.anm", "kb_walk.anm", "kb_punch.anm", "kb_stomp.anm", "kb_roar.anm", "kb_die.anm"}, 7.0f, 0.32f, 28, 22, 0.45f, 0.5f,
     "ka_pnch1", "ka_jump2", "ka_roar1", "ka_mdht1", "ka_deth1"},
    {{"offspring_stand.anm", "offspring_run.anm", "offspring_claw.anm", "offspring_bite.anm", "offspring_roar.anm", "offspring_die.anm"},
     9.0f, 0.30f, 14, 20, 0.45f, 0.55f, "of_claw1", "of_claw2", "of_roar1", "of_dhit1", "of_deth1"},
    {{"charger_stand_agro.anm", "charger_run_fast.anm", "charger_attack.anm", "charger_run_fast_open.anm", "charger_sniff.anm",
      "charger_mid_run_die.anm"},
     6.0f, 0.45f, 18, 30, 0.5f, 0.0f, "ca_snort", "ca_roar", "ca_roar", "ca_injr1", "ca_deat1"},
    {{"sonak_idle.anm", "sonak_walk.anm", "sonak_roar_attack.anm", "sonak_stomp.anm", "sonak_idle_roar.anm", "sonak_die.anm"}, 5.0f, 0.55f, 16,
     24, 0.45f, 0.5f, "so_atst1", "so_snrt1", "so_roar1", "so_injr1", "so_deat1"},
};
}  // namespace

void GameApp::setupBossAI(CharacterInst& ci, int kind) {
    if (!ci.model || kind < 1 || kind > 4 || !assetWorld_) return;
    ci.bossKind = kind;
    ci.bossKb = kind == 1;
    for (int k = 0; k < 6; ++k) {
        const int id = ci.model->addAnimation(assetWorld_->fs(), kBoss[kind].clips[k]);
        ci.bossClip[k] = std::max(0, id);
    }
    ci.bossState = 0;
    ci.bossCool = 2.0f;
}

// kstory5_4 ("Kabuto, meet Kabuto. DEFEAT KABUTO!"): the mission places only the player's Kabuto
// (id 1) and a marker (id 3); the rival giant is ours, standing at that marker.
bool GameApp::spawnRivalKabuto(const Vec3& pos, float yaw) { return spawnBoss(1, "kb_l0", "kb_stand.anm", pos, yaw, 2500.0f) >= 0; }

int GameApp::spawnBoss(int kind, const char* model, const char* idle, const Vec3& pos, float yaw, float hp) {
    if (!assetWorld_) return -1;
    auto m = std::make_unique<SkinnedModel>();
    if (!m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, model, idle)) return -1;
    CharacterInst ci;
    ci.model = std::move(m);
    setupBossAI(ci, kind);
    ci.model->pose(renderer_, 0.0f);
    Vec3 mn, mx;
    ci.model->posedBounds(mn, mx);
    ci.pos = pos;
    ci.yaw = yaw;
    ci.lift = ci.restLift = -mn.y;
    ci.boss = true;
    ci.hp = ci.bossMaxHp = hp;
    ci.bossR = std::max(1.5f, 0.12f * (mx.y - mn.y));
    if (kind != 1) ci.bossR = std::max(1.0f, 0.3f * std::max(mx.x - mn.x, mx.z - mn.z));
    ci.bossH = std::max(2.0f, mx.y - mn.y);
    ci.objective = int(objectives_.size());
    objectives_.push_back({2, pos, pos});
    chars_.push_back(std::move(ci));
    missionHostiles_ = true;
    LOGI("Objective: defeat %s (%.0f hp, %.1f m tall)", model, hp, chars_.back().bossH);
    return int(chars_.size()) - 1;
}

void GameApp::updateBosses(float dt) {
    for (size_t idx = 0; idx < chars_.size(); ++idx) {
        CharacterInst& ci = chars_[idx];
        if (!ci.boss) continue;
        if (ci.bossKind > 0) {
            const BossSpec& sp = kBoss[ci.bossKind];
            ci.bossT += dt;
            ci.bossClipT += dt;
            float gy;
            if (assetWorld_ && assetWorld_->terrain().heightAt(ci.pos.x, ci.pos.z, gy)) ci.pos.y = gy;
            int clip = 0;
            bool once = false;
            if (ci.dieT >= 0) {
                ci.dieT += dt;
                clip = 5;
                once = true;
                ci.bossClipT = ci.dieT;
                // Kabuto lies where he fell; the beasts sink away after a while.
                if (ci.bossKind != 1 && ci.dieT > 5.0f) {
                    ci.lift = ci.restLift - (ci.dieT - 5.0f) * 0.5f;
                    if (ci.dieT > 5.0f + ci.bossH / 0.5f) ci.visible = false;
                }
            } else if (playMode_ && !dying_) {
                Vec3 to = player_.position - ci.pos;
                to.y = 0;
                const float d = length(to);
                const float reach = sp.reach * ci.bossH + player_.radius + 0.5f;
                const float want = std::atan2(-to.x, -to.z);
                const Vec3 fwd{-std::sin(ci.yaw), 0, -std::cos(ci.yaw)};
                ci.bossCool = std::max(0.0f, ci.bossCool - dt);
                auto setState = [&](int st) { ci.bossState = st; ci.bossClipT = 0; ci.bossHit = false; };
                auto clipLen = [&](int c) { return std::max(0.2f, ci.model->animDuration(ci.bossClip[c])); };
                auto strike = [&](float dmg, const Vec3& from, DeathCause cause) {
                    hitFrom_ = from;
                    hitDirTimer_ = 1.2f;
                    damagePlayer(dmg, cause);
                    LOGI("Boss (kind %d) hits: health %.0f", ci.bossKind, playerHealth_);
                };
                auto walkable = [&](const Vec3& p) {  // not into the sea or up a cliff
                    float g;
                    return assetWorld_ && assetWorld_->terrain().heightAt(p.x, p.z, g) && g > player_.seaLevel - 1.0f &&
                           std::fabs(g - ci.pos.y) < 3.0f;
                };
                switch (ci.bossState) {
                    case 0:  // approach
                        ci.yaw += std::max(-1.8f * dt, std::min(1.8f * dt, angleDelta(ci.yaw, want)));
                        if (ci.bossKind == 3 && d > 14.0f && d < 80.0f && ci.bossCool <= 0 && std::fabs(angleDelta(ci.yaw, want)) < 0.3f) {
                            // The Charger lowers its head and charges at where the player is.
                            setState(5);
                            ci.bossChargeDir = to * (1.0f / d);
                            sfx(sp.sAttack2, ci.pos, 1.0f);
                        } else if (d > reach) {
                            const Vec3 np = ci.pos + fwd * std::min(d - reach * 0.8f, sp.speed * dt);
                            if (walkable(np)) ci.pos = np;
                            clip = 1;
                            if (d > 80.0f && ci.bossCool <= 0) { setState(4); ci.bossCool = 12.0f; sfx(sp.sRoar, ci.pos, 1.0f); }
                        } else if (ci.bossCool <= 0) {
                            setState(ci.bossKind == 3 || frand() < 0.6f ? 2 : 3);
                            sfx(ci.bossState == 2 ? sp.sAttack1 : sp.sAttack2, ci.pos, 1.0f);
                        }
                        break;
                    case 2:  // the main blow (punch, claw, gore)
                        clip = 2;
                        once = true;
                        if (!ci.bossHit && ci.bossClipT > clipLen(2) * sp.at1) {
                            ci.bossHit = true;
                            if (d < reach + 1.5f) strike(sp.hit1, ci.pos + Vec3{0, 0.6f * ci.bossH, 0}, DeathCause::Shot);
                        }
                        if (ci.bossClipT > clipLen(2)) { setState(0); ci.bossCool = 1.0f; }
                        break;
                    case 3:  // the second (Kabuto's stomp: a quake; the Offspring's bite)
                        clip = 3;
                        once = true;
                        if (!ci.bossHit && ci.bossClipT > clipLen(3) * sp.at2) {
                            ci.bossHit = true;
                            if (ci.bossKind == 1) {
                                const Vec3 foot = ci.pos + fwd * (0.15f * ci.bossH);
                                dustBurst(foot, 0.3f * ci.bossH, 20);
                                sfx("ka_stomp", foot, 1.0f);
                                haptics_.push_back({kLeft, 0.8f, 0.25f});
                                haptics_.push_back({kRight, 0.8f, 0.25f});
                                if (length(Vec3{player_.position.x - foot.x, 0, player_.position.z - foot.z}) < 0.45f * ci.bossH + player_.radius)
                                    strike(sp.hit2, foot, DeathCause::Blast);
                            } else if (d < reach + 1.0f) {
                                strike(sp.hit2, ci.pos + Vec3{0, 0.5f * ci.bossH, 0}, DeathCause::Bitten);
                            }
                        }
                        if (ci.bossClipT > clipLen(3)) { setState(0); ci.bossCool = 1.6f; }
                        break;
                    case 4:  // roar
                        clip = 4;
                        once = true;
                        if (ci.bossClipT > std::min(3.0f, clipLen(4))) setState(0);
                        break;
                    case 5: {  // the Charger's charge: straight on, fast, until it hits or runs out
                        clip = 3;
                        const float spd = 17.0f;
                        const Vec3 np = ci.pos + ci.bossChargeDir * (spd * dt);
                        ci.yaw = std::atan2(-ci.bossChargeDir.x, -ci.bossChargeDir.z);
                        const bool blocked = !walkable(np);
                        if (!blocked) ci.pos = np;
                        if (frand() < dt * 8.0f) dustBurst(ci.pos, 1.0f, 2);
                        if (!ci.bossHit && d < reach + 1.0f) {
                            ci.bossHit = true;
                            strike(sp.hit2, ci.pos + Vec3{0, 0.5f * ci.bossH, 0}, DeathCause::Blast);
                            // Thrown aside.
                            player_.velocity.x += ci.bossChargeDir.x * 10.0f;
                            player_.velocity.z += ci.bossChargeDir.z * 10.0f;
                            player_.velocity.y = std::max(player_.velocity.y, 5.0f);
                            player_.onGround = false;
                        }
                        if (blocked || ci.bossHit || ci.bossClipT > 3.5f) { setState(0); ci.bossCool = 3.0f; }
                        break;
                    }
                }
            }
            if (length(ci.pos - player_.position) < 300.0f) {
                const float len = std::max(0.1f, ci.model->animDuration(ci.bossClip[clip]));
                const float t = once ? std::min(ci.bossClipT, len - 0.01f) : std::fmod(ci.bossClipT, len);
                ci.model->poseBlend(renderer_, ci.bossClip[clip], t, ci.bossClip[clip], t, 0.0f);
            }
            continue;
        }
        if (ci.dieT < 0) continue;
        // Down: it sinks into the ground and is gone.
        ci.dieT += dt;
        ci.lift = ci.restLift - std::max(0.0f, ci.dieT - 1.0f) * 0.6f;
        if (ci.dieT > 1.0f + ci.bossH / 0.6f) ci.visible = false;
    }
}

void GameApp::updateSams(float dt) {
    if (!assetWorld_ || !playMode_ || dying_) return;
    const auto& objs = assetWorld_->worldObjects();
    // The player counts as airborne a few metres up (jetpack flight, a big jump).
    float g = player_.position.y;
    if (player_.groundHeight) g = player_.groundHeight(player_.position.x, player_.position.z);
    const bool airborne = !player_.onGround && player_.position.y - g > 3.0f;
    int live = 0;
    for (const auto& r : rockets_) live += r.kind == 2 && r.life < 1e9f;
    for (size_t k = 0; k < destruct_.items.size(); ++k) {
        const Destructible& it = destruct_.items[k];
        if (!it.alive() || objs[size_t(it.object)].name != "guard_sam") continue;
        auto c = std::find_if(samCool_.begin(), samCool_.end(), [&](const std::pair<int, float>& p) { return p.first == int(k); });
        if (c == samCool_.end()) { samCool_.push_back({int(k), 2.0f}); c = samCool_.end() - 1; }
        c->second -= dt;
        const Vec3 top = it.base + Vec3{0, it.height * 0.9f, 0};
        const float d = length(player_.position - top);
        if (!airborne || d > 160.0f || c->second > 0 || live >= 3) continue;
        c->second = 4.5f;
        Rocket r;
        r.kind = 2;
        r.pos = top;
        r.vel = normalize(Vec3{0, 1.0f, 0} + normalize(player_.position - top) * 0.6f) * 14.0f;  // up out of the tube
        r.life = 0;
        rockets_.push_back(r);
        ++live;
        sfx("pr_wsmil", top, 1.0f);
        LOGI("SAM launches at the airborne player (%.0f m)", d);
    }
}

}  // namespace gvr
