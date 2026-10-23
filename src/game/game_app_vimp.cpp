// GameApp: Vimp herds, Vimp meat and the "Vimp meat proximity" mission trigger. Same class; see
// game_app.h ("Vimp herds") and vimp.h.
#include "game/game_app.h"
#include "game/player.h"

#include <algorithm>
#include <cmath>

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
        const float tn = -b - std::sqrt(disc);
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
            const float tn = -b - std::sqrt(disc);
            if (tn < 0 || tn > maxD || (hit && tn >= t)) continue;
            t = tn;
            index = int(i);
            hit = true;
        }
    }
    return hit;
}

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
    bleed(at, dir, ci.hp <= 0, std::max(1.0f, ci.bossH / 2.0f));
    if (ci.bossKb && ci.hp > 0 && frand() < 0.15f) sfx(frand() < 0.5f ? "ka_mdht1" : "ka_hvht1", at, 1.0f);
    if (ci.hp <= 0) {
        ci.dieT = 0;
        blood_.pool(ci.pos, std::max(1.5f, ci.bossR * 2.0f));
        sfx(ci.bossKb ? "ka_deth1" : "ex_bdyl1", at, 1.0f, ci.bossKb ? 1.0f : 0.8f);
        if (ci.objective >= 0) completeObjective(ci.objective);
        LOGI("Mission target killed (character %d)", i);
    }
}

// kstory5_4 ("Kabuto, meet Kabuto. DEFEAT KABUTO!"): the mission places only the player's Kabuto
// (id 1) and a marker (id 3); the rival giant is ours, standing at that marker.
bool GameApp::spawnRivalKabuto(const Vec3& pos, float yaw) {
    if (!assetWorld_) return false;
    auto m = std::make_unique<SkinnedModel>();
    if (!m->load(assetWorld_->fs(), *assetWorld_->models(), renderer_, "kb_l0", "kb_stand.anm")) return false;
    CharacterInst ci;
    static const char* kc[6] = {"kb_stand.anm", "kb_walk.anm", "kb_punch.anm", "kb_stomp.anm", "kb_roar.anm", "kb_die.anm"};
    for (int k = 0; k < 6; ++k) ci.bossClip[k] = std::max(0, k == 0 ? 0 : m->addAnimation(assetWorld_->fs(), kc[k]));
    m->pose(renderer_, 0.0f);
    Vec3 mn, mx;
    m->posedBounds(mn, mx);
    ci.pos = pos;
    ci.yaw = yaw;
    ci.lift = ci.restLift = -mn.y;
    ci.bossKb = ci.boss = true;
    ci.hp = ci.bossMaxHp = 2500.0f;  // kb_l0's type hit points
    ci.bossR = std::max(1.5f, 0.12f * (mx.y - mn.y));
    ci.bossH = std::max(2.0f, mx.y - mn.y);
    ci.objective = int(objectives_.size());
    objectives_.push_back({2, pos, pos});
    ci.model = std::move(m);
    chars_.push_back(std::move(ci));
    missionHostiles_ = true;
    LOGI("Objective: defeat the rival Kabuto (%.1f m tall)", chars_.back().bossH);
    return true;
}

void GameApp::updateBosses(float dt) {
    for (size_t idx = 0; idx < chars_.size(); ++idx) {
        CharacterInst& ci = chars_[idx];
        if (!ci.boss) continue;
        if (ci.bossKb) {
            // --- The other Kabuto.
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
            } else if (playMode_ && !dying_) {
                Vec3 to = player_.position - ci.pos;
                to.y = 0;
                const float d = length(to);
                const float reach = 0.32f * ci.bossH + player_.radius;
                const float want = std::atan2(-to.x, -to.z);
                ci.bossCool = std::max(0.0f, ci.bossCool - dt);
                auto setState = [&](int st) { ci.bossState = st; ci.bossClipT = 0; ci.bossHit = false; };
                const float dur = ci.model->animDuration(ci.bossClip[ci.bossState == 2 ? 2 : (ci.bossState == 3 ? 3 : 4)]);
                switch (ci.bossState) {
                    case 0:  // approach
                        ci.yaw += std::max(-1.5f * dt, std::min(1.5f * dt, angleDelta(ci.yaw, want)));
                        if (d > reach) {
                            const float step = std::min(d - reach * 0.8f, 7.0f * dt);
                            ci.pos = ci.pos + Vec3{-std::sin(ci.yaw), 0, -std::cos(ci.yaw)} * step;
                            clip = 1;
                            if (d > 80.0f && ci.bossCool <= 0) { setState(4); ci.bossCool = 12.0f; sfx(frand() < 0.5f ? "ka_roar1" : "ka_roar3", ci.pos, 1.0f); }
                        } else if (ci.bossCool <= 0) {
                            setState(frand() < 0.65f ? 2 : 3);
                            sfx(ci.bossState == 2 ? "ka_pnch1" : "ka_jump2", ci.pos, 1.0f);
                        }
                        break;
                    case 2:  // punch: lands at 45 %
                        clip = 2;
                        once = true;
                        if (!ci.bossHit && ci.bossClipT > dur * 0.45f) {
                            ci.bossHit = true;
                            if (d < reach + 2.0f) {
                                hitFrom_ = ci.pos + Vec3{0, 0.6f * ci.bossH, 0};
                                hitDirTimer_ = 1.2f;
                                damagePlayer(28.0f, DeathCause::Shot);
                                LOGI("Rival Kabuto punches: health %.0f", playerHealth_);
                                sfx("ka_hvht1", player_.position, 1.0f);
                            }
                        }
                        if (ci.bossClipT > dur) { setState(0); ci.bossCool = 1.2f; }
                        break;
                    case 3: {  // stomp: a quake at its front foot at 50 %
                        clip = 3;
                        once = true;
                        if (!ci.bossHit && ci.bossClipT > dur * 0.5f) {
                            ci.bossHit = true;
                            const Vec3 foot = ci.pos + Vec3{-std::sin(ci.yaw), 0, -std::cos(ci.yaw)} * (0.15f * ci.bossH);
                            dustBurst(foot, 0.3f * ci.bossH, 20);
                            sfx("ka_stomp", foot, 1.0f);
                            haptics_.push_back({kLeft, 0.8f, 0.25f});
                            haptics_.push_back({kRight, 0.8f, 0.25f});
                            if (length(Vec3{player_.position.x - foot.x, 0, player_.position.z - foot.z}) < 0.45f * ci.bossH + player_.radius) {
                                hitFrom_ = foot;
                                hitDirTimer_ = 1.2f;
                                damagePlayer(22.0f, DeathCause::Blast);
                                LOGI("Rival Kabuto stomps: health %.0f", playerHealth_);
                            }
                        }
                        if (ci.bossClipT > dur) { setState(0); ci.bossCool = 2.0f; }
                        break;
                    }
                    case 4:  // roar
                        clip = 4;
                        once = true;
                        if (ci.bossClipT > std::min(3.0f, dur)) setState(0);
                        break;
                }
            }
            if (length(ci.pos - player_.position) < 300.0f) {
                const float len = ci.model->animDuration(ci.bossClip[clip]);
                const float t = once ? std::min(ci.bossClipT, len - 0.01f) : std::fmod(ci.bossClipT, std::max(0.1f, len));
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

}  // namespace gvr
