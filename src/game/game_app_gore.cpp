// GameApp: Gore: blood, gibs, corpses that can be shot apart. Same class; see game_app.h.
// (Split out of game_app.cpp, 2026-10-28: moved as it was.)
#include "game/game_app.h"
#include "game/hero_clips.h"
#include "core/log.h"
#include "core/prof.h"
#include "scene/asset_world.h"
#include "audio/mixer.h"
#include "audio/music.h"
#include "assets/gtext.h"
#include "assets/skin_anim.h"
#include "game/cutscene.h"
#include "game/arm_ik.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>

namespace gvr {
void GameApp::spawnGore(const Vec3& pos, const Vec3& dir, float scale) {
    // The original gore pieces (rip_gore_head, rip_gore_arm x2) thrown along the killing shot.
    blood_.burst(pos + Vec3{0, 0.5f * scale, 0}, dir + Vec3{0, 0.8f, 0}, 28, 5.5f, 0.08f * std::max(0.7f, scale));
    blood_.pool(pos, 1.2f * std::max(0.7f, scale));
    const RenderModel* parts[3] = {goreHead_, goreArm_, goreArm_};
    Vec3 push{dir.x, 0, dir.z};
    if (length(push) > 1e-3f) push = normalize(push);
    for (const RenderModel* m : parts) {
        if (!m) continue;
        Gib g;
        g.model = m;
        g.pos = pos + Vec3{0, 0.5f * scale, 0};
        g.vel = push * (3.0f + 2.0f * frand()) + Vec3{(frand() - 0.5f) * 4.0f, 4.0f + 3.0f * frand(), (frand() - 0.5f) * 4.0f};
        g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
        g.angle = frand() * 6.28f;
        g.spin = 4.0f + 6.0f * frand();
        g.scale = scale * 0.8f;
        g.life = 0;
        gibs_.push_back(g);
    }
}

void GameApp::initGore(AssetWorld* w, ModelCache* mc) {
    if (!goreQuad_) {
        MeshData q;
        const float c[4][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        for (const auto& p : c) {
            Vertex v{};
            v.pos[0] = p[0];
            v.pos[1] = p[1];
            v.normal[2] = 1;
            v.uv[0] = p[0] + 0.5f;
            v.uv[1] = 0.5f - p[1];
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
            q.vertices.push_back(v);
        }
        q.indices = {0, 1, 2, 0, 2, 3};
        goreQuad_ = renderer_.uploadMesh(q);
        bloodTex_[0] = mc->texture("efx_splat");
        bloodTex_[1] = mc->texture("efx_splat2");
        bloodTex_[2] = mc->texture("bloodblob");  // soft round: pools and clouds in the water
        sprayTex_ = mc->texture("efx_spray");
        dropTex_ = mc->texture("efx_blood");
        const char* rc[4] = {"rpman_chunk_head.gbs", "rpman_chunk_body.gbs", "rpman_chunk_main.gbs", "rpman_chunk_hand.gbs"};
        for (int k = 0; k < 4; ++k) reaperChunk_[k] = mc->get(rc[k], "", /*dynamicLit=*/true);
        for (int k = 0; k < 6; ++k) meccChunk_[k] = mc->get("MC_chunk" + std::to_string(k + 1) + ".gbs", "", true);
        for (int k = 0; k < 18; ++k) {  // three swarms at a time
            auto m = std::make_unique<SkinnedModel>();
            if (!m->load(w->fs(), *mc, renderer_, "cleaner_l0", "cleaner_walk.anm")) break;
            m->addAnimation(w->fs(), "cleaner_eat.anm");
            m->addAnimation(w->fs(), "cleaner_jump.anm");
            cleanerPool_.push_back(std::move(m));
        }
        LOGI("Gore: blood textures %u %u %u, %zu Cleaners, Reaper chunks %d", bloodTex_[0], bloodTex_[1], bloodTex_[2],
             cleanerPool_.size(), reaperChunk_[0] != nullptr);
    }
    auto ground = [w](float x, float z) {
        float y;
        return w->terrain().heightAt(x, z, y) ? y : -20.0f;
    };
    blood_ = BloodSystem{};
    blood_.groundHeight = ground;
    blood_.seaLevel = 0.0f;
    cleaners_.cleaners.clear();
    cleaners_.groundHeight = [w](float x, float z) {
        float y;
        return std::max(w->terrain().heightAt(x, z, y) ? y : -20.0f, -0.3f);
    };
    enemies_.params.corpsesPersist = true;
    if (const char* m = std::getenv("GVR_MARKSMEN")) enemies_.params.marksmen = float(std::atof(m));  // test hook: share of marksmen
    if (const char* c = std::getenv("GVR_CROUCHERS")) enemies_.params.crouchers = float(std::atof(c));  // test hook: share crouching to fire
    if (const char* c = std::getenv("GVR_COVER")) enemies_.params.coverShare = float(std::atof(c));  // test hook: chance of seeking cover
    creatures_.params.corpsesPersist = true;
}

void GameApp::bloodInWater(const Vec3& at, float size) {
    blood_.pool(Vec3{at.x + (frand() - 0.5f) * 0.8f, -0.5f, at.z + (frand() - 0.5f) * 0.8f}, size);
}

void GameApp::bleed(const Vec3& at, const Vec3& dir, bool kill, float scale) {
    // The blood comes out of the far side of a wound and sprays up a little.
    blood_.burst(at, dir + Vec3{0, 0.4f, 0}, kill ? 16 : 6, kill ? 4.0f : 3.0f, 0.07f * std::max(0.6f, scale));
}

void GameApp::gibCorpse(const Vec3& pos, float yaw, const Vec3& dir, bool reaper, float scale) {
    // Either bursts (pieces flung far, a big spray) or falls apart (pieces tumble close by).
    const bool burst = frand() < 0.5f;
    const float k = burst ? 1.0f : 0.35f;
    const RenderModel* reap[4] = {reaperChunk_[0], reaperChunk_[1], reaperChunk_[2], reaperChunk_[3]};
    const RenderModel* rip[3] = {goreHead_, goreArm_, goreArm_};
    const int n = reaper ? 4 : 3;
    Vec3 push{dir.x, 0, dir.z};
    if (length(push) > 1e-3f) push = normalize(push);
    for (int i = 0; i < n; ++i) {
        const RenderModel* m = reaper ? reap[i] : rip[i];
        if (!m) continue;
        Gib g;
        g.model = m;
        const Vec3 along = Quat::fromAxisAngle({0, 1, 0}, yaw).rotate(Vec3{0, 0, (float(i) - 1.5f) * 0.35f * scale});
        g.pos = pos + along + Vec3{0, 0.3f * scale, 0};
        g.vel = (push * (3.0f + 2.0f * frand()) + Vec3{(frand() - 0.5f) * 4.0f, 3.0f + 3.0f * frand(), (frand() - 0.5f) * 4.0f}) * k;
        g.axis = normalize(Vec3{frand() - 0.5f, frand() - 0.5f, frand() - 0.5f} + Vec3{0.01f, 0, 0});
        g.angle = frand() * 6.28f;
        g.spin = (4.0f + 6.0f * frand()) * k;
        g.scale = reaper ? 1.0f : scale * 0.8f;
        g.life = 0;
        gibs_.push_back(g);
        blood_.burst(g.pos, g.vel + Vec3{0, 1, 0}, 4, 2.5f, 0.07f);
    }
    blood_.burst(pos + Vec3{0, 0.4f * scale, 0}, Vec3{0, 1, 0} + push, burst ? 30 : 14, burst ? 6.0f : 3.0f, 0.08f);
    blood_.pool(pos, 1.6f * std::max(0.7f, scale));
    sfx(frand() < 0.5f ? "ex_bdyl1" : "ex_bdyl2", pos, 1.0f, 0.9f + 0.2f * frand());
    LOGI("A body %s", burst ? "bursts" : "falls apart");
}

bool GameApp::corpseRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& target) const {
    // Bodies lying on the ground: the box of the posed body (its death pose) in the body's frame.
    bool hit = false;
    float best = maxD;
    auto test = [&](const Vec3& pos, float yaw, const Vec3& mn, const Vec3& mx, int id) {
        const Quat inv = Quat::fromAxisAngle({0, 1, 0}, -yaw);
        const Vec3 lo = inv.rotate(o - pos), ld = inv.rotate(d);
        float t0 = 0, t1 = best;
        const float ro[3] = {lo.x, lo.y, lo.z}, rd[3] = {ld.x, ld.y, ld.z}, bmn[3] = {mn.x, mn.y, mn.z}, bmx[3] = {mx.x, mx.y, mx.z};
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(rd[a]) < 1e-6f) {
                if (ro[a] < bmn[a] || ro[a] > bmx[a]) return;
                continue;
            }
            float ta = (bmn[a] - ro[a]) / rd[a], tb = (bmx[a] - ro[a]) / rd[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return;
        }
        if (t0 < best) { best = t0; target = id; hit = true; }
    };
    for (size_t i = 0; i < enemies_.enemies.size(); ++i) {
        const Enemy& e = enemies_.enemies[i];
        if (e.state != EnemyState::Dead || e.stateTime < 0.6f || e.slot < 0) continue;
        Vec3 mn, mx;
        enemyPool_[size_t(e.slot)]->posedBounds(mn, mx);
        const float lift = slotLift_[size_t(e.slot)];
        test(e.pos + Vec3{0, lift, 0}, e.yaw, mn, mx, kCorpseEnemy + int(i));
    }
    for (size_t i = 0; i < creatures_.creatures.size(); ++i) {
        const Creature& c = creatures_.creatures[i];
        if (c.state != CreatureState::Dead || c.exploded || c.stateTime < 0.6f || c.slot < 0 || size_t(c.slot) >= chars_.size()) continue;
        const CharacterInst& ci = chars_[size_t(c.slot)];
        Vec3 mn, mx;
        ci.model->posedBounds(mn, mx);
        test(c.pos + Vec3{0, ci.lift, 0}, c.yaw, mn * ci.scale, mx * ci.scale, kCorpseCreature + int(i));
    }
    if (hit) t = best;
    return hit;
}

void GameApp::shootCorpse(int target, const Vec3& p, const Vec3& dir) {
    if (target >= kCorpseCreature) {
        Creature& c = creatures_.creatures[size_t(target - kCorpseCreature)];
        bleed(p, dir, false, c.scale);
        sfx("ex_bdys1", p, 0.5f, 1.1f + 0.2f * frand());
        c.corpseHp -= 25.0f;
        if (c.corpseHp <= 0 && !c.gibbed) {
            c.gibbed = true;
            gibCorpse(c.pos, c.yaw, dir, false, c.scale);
        }
        return;
    }
    Enemy& e = enemies_.enemies[size_t(target - kCorpseEnemy)];
    bleed(p, dir, false, 1.0f);
    sfx("ex_bdys1", p, 0.5f, 1.0f + 0.2f * frand());
    e.corpseHp -= 25.0f;
    if (e.corpseHp <= 0 && !e.gibbed) {
        e.gibbed = true;
        gibCorpse(e.pos, e.yaw, dir, true, 1.0f);
    }
}

void GameApp::updateGore(float dt) {
    blood_.update(dt);
    // Bodies lying a while attract the Cleaners (within sight of the player; far ones just lie).
    auto consider = [&](uint32_t uid, const Vec3& pos, float age, bool& called, float size) {
        if (called || age < 14.0f || length(pos - player_.position) > 60.0f || cleanerPool_.empty()) return;
        float h;
        if (pos.y < -0.4f || !assetWorld_->terrain().heightAt(pos.x, pos.z, h)) return;  // in the sea: the piranhas' (below)
        // A swarm: five or six to a Reaper, three or four to a small Ripper (device test 20: more).
        const size_t busy = cleaners_.cleaners.size();
        const int want = std::min(int(cleanerPool_.size() - std::min(busy, cleanerPool_.size())), (size > 0.8f ? 5 : 3) + int(frand() * 1.99f));
        if (want < 2) return;
        called = true;
        cleaners_.call(uid, pos, want, frand() * 6.28f);
        LOGI("Cleaners called to body %u", uid);
    };
    for (auto& e : enemies_.enemies)
        if (e.state == EnemyState::Dead && !e.gibbed) consider(e.uid, e.pos, e.stateTime, e.cleanersCalled, 1.0f);
    for (auto& c : creatures_.creatures)
        if (c.state == CreatureState::Dead && !c.exploded && !c.gibbed) consider(0x80000000u | c.uid, c.pos, c.stateTime, c.cleanersCalled, c.scale);
    // Bodies in deep water: the piranhas take them (blood in the water, gone in a few seconds).
    for (auto& e : enemies_.enemies)
        if (e.state == EnemyState::Dead && !e.gibbed && e.pos.y < -0.4f && e.stateTime > 1.0f) {
            e.eaten += dt / 6.0f;
            if (frand() < dt * 2.0f) bloodInWater(e.pos, 0.7f);
        }
    for (auto& c : creatures_.creatures)
        if (c.state == CreatureState::Dead && !c.exploded && !c.gibbed && c.pos.y < -0.4f && c.stateTime > 1.0f) {
            c.eaten += dt / 5.0f;
            if (frand() < dt * 2.0f) bloodInWater(c.pos, 0.5f);
        }
    auto bodyAt = [this](uint32_t id, Vec3& at) {
        if (id & 0x80000000u) {
            for (const auto& c : creatures_.creatures)
                if (c.uid == (id & 0x7fffffffu)) {
                    if (c.state != CreatureState::Dead || c.gibbed || c.eaten >= 1.0f) return false;
                    at = c.pos;
                    return true;
                }
            return false;
        }
        for (const auto& e : enemies_.enemies)
            if (e.uid == id) {
                if (e.state != EnemyState::Dead || e.gibbed || e.eaten >= 1.0f) return false;
                at = e.pos;
                return true;
            }
        return false;
    };
    eatenNow_.clear();
    cleanerEvents_.clear();
    cleaners_.update(dt, bodyAt, eatenNow_, cleanerEvents_);
    for (const auto& pr : eatenNow_) {
        if (pr.first & 0x80000000u) {
            for (auto& c : creatures_.creatures)
                if (c.uid == (pr.first & 0x7fffffffu)) c.eaten = std::min(1.0f, c.eaten + pr.second * (1.6f / std::max(0.5f, c.scale)));
        } else {
            for (auto& e : enemies_.enemies)
                if (e.uid == pr.first) {
                    e.eaten = std::min(1.0f, e.eaten + pr.second);
                    if (e.eaten >= 1.0f) LOGI("Reaper body %u eaten by the Cleaners", e.uid);
                }
        }
    }
    for (const auto& ev : cleanerEvents_) {
        if (ev.kind == CleanerEvent::StartEat) {
            LOGI("Cleaners eating body %u", ev.target);
            sfx("cl_eatst", ev.pos, 0.9f);
            if (audio_) eatLoops_.push_back({ev.target, audio_->play("cl_eatlp", ev.pos, 0.7f, 1.0f, true)});
        }
    }
    // Eating finished (or the body went another way): stop the loop, a last gulp.
    for (auto it = eatLoops_.begin(); it != eatLoops_.end();) {
        Vec3 at;
        if (bodyAt(it->first, at)) { ++it; continue; }
        if (audio_) audio_->stop(it->second);
        LOGI("Body %u eaten", it->first);
        for (const auto& c : cleaners_.cleaners)
            if (c.target == it->first) {
                sfx("cl_eaten", c.pos, 0.9f);
                spawnBones(c.pos + Vec3{0, 0.3f, 0}, 3, Vec3{0, 0, 0}, 1.5f);  // (what's left)
                break;
            }
        it = eatLoops_.erase(it);
    }
    // Cleaner models: walk / eat / (jump while burrowing away).
    for (size_t k = 0; k < cleaners_.cleaners.size() && k < cleanerPool_.size(); ++k) {
        const Cleaner& c = cleaners_.cleaners[k];
        if (length(c.pos - player_.position) > 80.0f) continue;
        const int clip = c.state == CleanerState::Eat ? 1 : (c.sink > 0 ? 2 : 0);
        cleanerPool_[k]->poseBlend(renderer_, clip, c.anim * (clip == 0 ? 1.6f : 1.0f), clip, c.anim, 0.0f);
    }
}

void GameApp::renderGore() {
    // Blood splats, drops and sprays are drawn in batches (renderFx); the Cleaners here.
    const Vec3 eye = renderer_.currentView().eyePosition;
    for (size_t k = 0; k < cleaners_.cleaners.size() && k < cleanerPool_.size(); ++k) {
        const Cleaner& c = cleaners_.cleaners[k];
        if (length(c.pos - eye) > 80.0f) continue;
        cleanerPool_[k]->render(renderer_, Mat4::translation(c.pos) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, c.yaw)));
    }
}

}  // namespace gvr
