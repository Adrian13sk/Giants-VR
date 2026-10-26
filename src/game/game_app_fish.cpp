// GameApp, rework: fish. The original ships xx_fish.gzp - the Shiner, a small fish with a swim
// clip - that no level we load places. Ours: two or three schools of Shiners swim just under the
// surface of the clear shallows (0.8-3 m deep, where the water shader lets the bed show) near the
// player. A school wanders slowly; its fish keep loose places round its centre. They dart away from
// a wading or swimming player, a hand put into the water, a splash (a shot, a stone, a body) or a
// blast, then gather again. Schools left far behind move to water near the player. Not for Kabuto
// (fish are specks at his size). Our own behaviour on the original art.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/game_app.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

namespace {
constexpr int kSchools = 3;
constexpr int kModels = 4;  // swim phases shared by the fish
}  // namespace

void GameApp::initFish(AssetWorld* w, ModelCache* mc) {
    shinerSchools_.clear();
    shinerModels_.clear();
    shinerSpawnT_ = 0;
    heldFish_[0] = heldFish_[1] = HeldFish{};
    looseFish_.clear();
    if (w) w->underwater = [this]() {
        renderFish(renderer_.currentView().eyePosition);
        renderPiranhas();
    };
    if (w && !w->ambientMarkers().empty()) {  // (the original's ambient zones in this level, by name)
        std::map<std::string, int> n;
        for (const auto& m : w->ambientMarkers()) ++n[m.name];
        std::string list;
        for (const auto& [k, v] : n) list += " " + k + " x" + std::to_string(v);
        LOGI("Ambient markers:%s", list.c_str());
    }
    if (!w || !w->hasTerrain() || !mc || std::getenv("GVR_NO_FISH")) return;
    for (int i = 0; i < kModels; ++i) {
        auto m = std::make_unique<SkinnedModel>();
        if (!m->load(w->fs(), *mc, renderer_, "Shiner", "Shiner_swim.anm")) {
            LOGW("Fish: Shiner unavailable");
            shinerModels_.clear();
            return;
        }
        if (i == 0) {
            Vec3 mn, mx;
            m->boundsAt(0, 0.0f, mn, mx);
            shinerLift_ = -0.5f * (mn.y + mx.y);
            LOGI("Fish: Shiner %.2f x %.2f x %.2f m", mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
        }
        m->pose(renderer_, 0.0f);
        shinerModels_.push_back(std::move(m));
    }
    shinerSchools_.resize(kSchools);
}

bool GameApp::placeSchool(ShinerSchool& sc, const Vec3& near, float minD, float maxD) {
    if (!assetWorld_ || player_.seaLevel < -1e8f) return false;
    const Terrain& t = assetWorld_->terrain();
    for (int tries = 0; tries < 24; ++tries) {
        const float a = frand() * 6.2832f, d = minD + frand() * (maxD - minD);
        const Vec3 c{near.x + std::cos(a) * d, 0, near.z + std::sin(a) * d};
        float g;
        if (!t.heightAt(c.x, c.z, g)) continue;
        const float depth = player_.seaLevel - g;
        if (depth < 0.8f || depth > 3.0f) continue;
        bool clash = false;
        for (const ShinerSchool& o : shinerSchools_) clash |= &o != &sc && o.live && length(o.centre - c) < 8.0f;
        if (clash) continue;
        sc.centre = sc.target = Vec3{c.x, player_.seaLevel - 0.35f, c.z};
        sc.wanderT = 0;
        sc.live = true;
        sc.fish.assign(size_t(7 + int(frand() * 6.0f)), ShinerFish{});
        for (ShinerFish& f : sc.fish) {
            f.offset = Vec3{(frand() - 0.5f) * 2.4f, -frand() * 0.25f, (frand() - 0.5f) * 2.4f};
            f.pos = sc.centre + f.offset;
            f.yaw = frand() * 6.2832f;
        }
        return true;
    }
    return false;
}

void GameApp::updateFish(float dt) {
    if (shinerModels_.empty() || !assetWorld_ || player_.seaLevel < -1e8f || hero_ == Hero::Kabuto) {
        heldFish_[0].active = heldFish_[1].active = false;  // (a held fish doesn't outlive the hero or the water)
        looseFish_.clear();
        return;
    }
    const Terrain& terr = assetWorld_->terrain();
    const float sea = player_.seaLevel;
    const Vec3 me = player_.position;
    auto depthAt = [&](float x, float z) {
        float g;
        return terr.heightAt(x, z, g) ? sea - g : 0.0f;
    };
    // Schools: (re)placed in shallows near the player; one far behind moves on.
    static bool testPending = std::getenv("GVR_FISH_TEST") && std::string(std::getenv("GVR_FISH_TEST")) != "hand";
    if (testPending && lastInput_.headValid) {
        testPending = false;
        // (tests: the first school in the first water 1-3 m deep straight ahead of the eyes)
        Vec3 f = rig_.toWorld(lastInput_.head).orientation.rotate(Vec3{0, 0, -1});
        f.y = 0;
        if (length(f) > 1e-3f) {
            f = normalize(f);
            for (float d = 2.0f; d < 40.0f; d += 0.5f) {
                const Vec3 p = me + f * d;
                shinerSchools_[0].live = false;
                if (depthAt(p.x, p.z) >= 0.85f && placeSchool(shinerSchools_[0], p + f * 0.4f, 0.0f, 0.0f)) {
                    LOGI("Fish: test school of %zu Shiners %.1f m ahead", shinerSchools_[0].fish.size(), d + 0.4f);
                    break;
                }
            }
        }
    }
    shinerSpawnT_ -= dt;
    if (shinerSpawnT_ <= 0) {
        shinerSpawnT_ = 3.0f;
        for (ShinerSchool& sc : shinerSchools_) {
            if (sc.live && length(Vec3{sc.centre.x - me.x, 0, sc.centre.z - me.z}) < 70.0f) continue;
            if (placeSchool(sc, me, 8.0f, 35.0f))
                LOGI("Fish: a school of %zu Shiners at (%.1f %.1f %.1f), %.1f m deep", sc.fish.size(), sc.centre.x, sc.centre.y, sc.centre.z,
                     depthAt(sc.centre.x, sc.centre.z));
            break;  // (one a time)
        }
    }
    // What frightens them: the player in the water, hands in the water, splashes, blasts.
    std::vector<std::pair<Vec3, float>> scares;  // position, radius
    std::vector<Vec3> stillHands;  // hands held still in the water: the fish come to look
    if (me.y < sea + 0.3f) scares.push_back({me, 3.5f});
    for (int h = 0; h < 2; ++h) {
        // A hand moving in the water scares them; one held still, they swim up to.
        const auto& hs = lastInput_.hand[h];
        if (!hs.gripPoseValid) { fishHandHave_[h] = false; continue; }
        const Vec3 p = rig_.toWorld(hs.gripPose).position;
        const float sp = fishHandHave_[h] && dt > 0 ? length(handVelocity(hs.gripPose.position, fishHandPrev_[h], dt) - player_.velocity) : 0.0f;
        fishHandPrev_[h] = hs.gripPose.position;
        fishHandHave_[h] = true;
        if (p.y < sea + 0.05f && sp > 0.7f) scares.push_back({p, 1.4f});
        else if (p.y < sea - 0.05f && sp < 0.35f) stillHands.push_back(p);
        // (tests: a school gathered round the right hand once it is in the water)
        static bool handTest = std::getenv("GVR_FISH_TEST") && std::string(std::getenv("GVR_FISH_TEST")) == "hand";
        if (handTest && h == 1) {
            static float tr = 0;
            if ((tr += dt) > 0.5f) { tr = 0; LOGI("Fish test: right hand %.2f m from the surface, feet %.2f", p.y - sea, me.y - sea); }
        }
        if (handTest && h == 1 && p.y < sea - 0.1f) {
            handTest = false;
            ShinerSchool& sc = shinerSchools_[0];
            sc.live = true;
            sc.centre = sc.target = p;
            sc.wanderT = 30.0f;
            sc.fish.assign(8, ShinerFish{});
            for (size_t i = 0; i < sc.fish.size(); ++i) {
                ShinerFish& f = sc.fish[i];
                f.offset = i == 0 ? Vec3{} : Vec3{(frand() - 0.5f) * 1.2f, 0, (frand() - 0.5f) * 1.2f};
                f.pos = p + f.offset;
                f.yaw = frand() * 6.2832f;
            }
            LOGI("Fish: test school round the right hand (%zu)", sc.fish.size());
        }
    }
    for (const auto& w : waterFx_)
        if (w.kind == 0 && w.age < 0.1f) scares.push_back({w.pos, 6.0f});
    for (const auto& b : blasts_)
        if (b.age < 0.1f) scares.push_back({b.pos, 15.0f});

    for (ShinerSchool& sc : shinerSchools_) {
        if (!sc.live) continue;
        // The school wanders: a new spot nearby, within the same shallows, now and then.
        sc.wanderT -= dt;
        if (sc.wanderT <= 0) {
            sc.wanderT = 4.0f + 4.0f * frand();
            for (int k = 0; k < 6; ++k) {
                const Vec3 c = sc.centre + Vec3{(frand() - 0.5f) * 10.0f, 0, (frand() - 0.5f) * 10.0f};
                const float d = depthAt(c.x, c.z);
                if (d > 0.7f && d < 3.5f) { sc.target = Vec3{c.x, sea - 0.35f, c.z}; break; }
            }
        }
        for (const Vec3& hp : stillHands)
            if (length(Vec3{hp.x - sc.centre.x, 0, hp.z - sc.centre.z}) < 6.0f && depthAt(hp.x, hp.z) > 0.3f) {
                sc.target = Vec3{hp.x, sea - 0.35f, hp.z};
                sc.wanderT = std::max(sc.wanderT, 1.5f);
            }
        Vec3 to = sc.target - sc.centre;
        const float tl = length(to);
        if (tl > 0.05f) sc.centre = sc.centre + to * (std::min(tl, 0.6f * dt) / tl);
        bool scared = false;
        for (ShinerFish& f : sc.fish) {
            // Flee: straight away from the nearest scare, fast, for a moment.
            for (const auto& s : scares) {
                Vec3 away{f.pos.x - s.first.x, 0, f.pos.z - s.first.z};
                const float d = length(away);
                if (d < s.second) {
                    if (f.flee <= 0) scared = true;
                    f.flee = 1.2f + 0.6f * frand();
                    away = d > 0.01f ? away * (1.0f / d) : Vec3{frand() - 0.5f, 0, frand() - 0.5f};
                    f.vel = away * (3.5f + 1.5f * frand());
                }
            }
            if (f.flee > 0) {
                f.flee -= dt;
                f.vel = f.vel * (1.0f - std::min(1.0f, 0.8f * dt));
            } else {
                // Back to its place round the centre, with a little drift of its own.
                const Vec3 want = sc.centre + f.offset + Vec3{std::sin(float(time_) * 0.7f + f.offset.x * 3.0f) * 0.3f, 0,
                                                              std::cos(float(time_) * 0.6f + f.offset.z * 3.0f) * 0.3f};
                const Vec3 v = (want - f.pos) * 1.2f;
                const float vl = length(v);
                const Vec3 vc = vl > 1.4f ? v * (1.4f / vl) : v;
                f.vel = f.vel + (vc - f.vel) * std::min(1.0f, dt * 2.5f);
            }
            Vec3 np = f.pos + f.vel * dt;
            // Stay in the water: under the surface, above the bed, out of the very shallow edge.
            const float d = depthAt(np.x, np.z);
            if (d < 0.35f) { np.x = f.pos.x; np.z = f.pos.z; f.vel = Vec3{-f.vel.x * 0.5f, 0, -f.vel.z * 0.5f}; }
            np.y = std::min(sea - 0.12f, std::max(sea - std::max(0.2f, d - 0.15f), np.y));
            f.pos = np;
            const Vec3 h{f.vel.x, 0, f.vel.z};
            if (length(h) > 0.08f) {
                const float want = std::atan2(-h.x, -h.z);
                float dy = want - f.yaw;
                while (dy > 3.1416f) dy -= 6.2832f;
                while (dy < -3.1416f) dy += 6.2832f;
                f.yaw += dy * std::min(1.0f, dt * 8.0f);
            }
        }
        if (scared) LOGI("Fish: a school scatters at (%.1f %.1f)", sc.centre.x, sc.centre.z);
    }
    updateFishCatch(dt);
    // The swim clip at a few phases (the fleeing ones use the faster ones).
    for (int i = 0; i < int(shinerModels_.size()); ++i)
        shinerModels_[size_t(i)]->pose(renderer_, float(time_) * (1.0f + 0.5f * float(i)) + float(i) * 0.37f);
}

void GameApp::renderFish(const Vec3& eye) {
    if (shinerModels_.empty() || hero_ == Hero::Kabuto || renderer_.inReflectionPass()) return;
    // In a hand: across the palm, wriggling.
    for (int h = 0; h < 2; ++h) {
        const HeldFish& hf = heldFish_[h];
        if (!hf.active || !lastInput_.hand[h].gripPoseValid) continue;
        const Pose g = rig_.toWorld(lastInput_.hand[h].gripPose);
        const float w = std::sin(hf.t * 17.0f) * 0.45f * std::max(0.3f, 1.0f - hf.t * 0.05f);
        shinerModels_[size_t(kModels - 1)]->render(
            renderer_, Mat4::translation(g.position + g.orientation.rotate(Vec3{0, -0.02f, -0.05f})) * Mat4::rotation(g.orientation) *
                           Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, 1.5708f + w)) * Mat4::translation(Vec3{0, shinerLift_, 0}));
    }
    // Let go: falling, flopping on land.
    for (const LooseFish& lf : looseFish_)
        shinerModels_[size_t(lf.landed ? kModels - 1 : 1)]->render(
            renderer_, Mat4::translation(lf.pos + Vec3{0, lf.landed ? 0.06f : 0.0f, 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, lf.yaw)) *
                           Mat4::rotation(Quat::fromAxisAngle({0, 0, 1}, lf.roll)) * Mat4::translation(Vec3{0, shinerLift_, 0}));
    for (const ShinerSchool& sc : shinerSchools_) {
        if (!sc.live || length(sc.centre - eye) > 45.0f) continue;
        int k = 0;
        for (const ShinerFish& f : sc.fish) {
            const int m = f.flee > 0 ? kModels - 1 - (k & 1) : (k % 2);
            ++k;
            shinerModels_[size_t(m)]->render(renderer_, Mat4::translation(f.pos + Vec3{0, shinerLift_, 0}) *
                                                            Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, f.yaw)));
        }
    }
}

void GameApp::updateFishCatch(float dt) {
    const bool can = playMode_ && firstPerson_ && !dying_ && !cutsceneActive() && !riding_;
    const float sea = player_.seaLevel;
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = lastInput_.hand[h];
        const bool g = c.squeeze > 0.7f;
        const bool edge = g && !fishGripWas_[h];
        fishGripWas_[h] = g;
        HeldFish& hf = heldFish_[h];
        if (hf.active) {
            hf.t += dt;
            if (c.gripPoseValid) {
                hf.lastPos = rig_.toWorld(c.gripPose).position;
                // (back from a tracking gap: no sample - the gap's movement in one frame would be a huge throw)
                if (!hf.tracked) hf.prev = c.gripPose.position;
                else if (dt > 0) hf.recent[hf.nRecent++ % 8] = handVelocity(c.gripPose.position, hf.prev, dt);
            }
            hf.tracked = c.gripPoseValid;
            if (hf.nRecent % 9 == 4) haptics_.push_back({h, 0.15f, 0.03f});  // (it wriggles)
            if (g && c.gripPoseValid && can) continue;
            // Let go: thrown at the hand's fastest recent speed.
            hf.active = false;
            LooseFish lf;
            lf.pos = c.gripPoseValid ? rig_.toWorld(c.gripPose).position : hf.lastPos;  // (lost tracking: where it was last seen)
            for (int k = 0; k < std::min(hf.nRecent, 8); ++k)
                if (length(hf.recent[k]) > length(lf.vel)) lf.vel = hf.recent[k];
            lf.yaw = frand() * 6.2832f;
            lf.spin = (frand() - 0.5f) * 12.0f;
            looseFish_.push_back(lf);
            LOGI("Fish: let go at %.1f m/s%s", length(lf.vel), lf.pos.y < sea ? " in the water" : "");
            continue;
        }
        if (!can || !edge || !c.gripPoseValid || stoneInHand(h) || heldBody_.hand == h || climbHold_[h]) continue;
        const Vec3 hp = rig_.toWorld(c.gripPose).position;
        if (hp.y > sea + 0.05f) continue;
        // The nearest fish within reach of the closing hand (a fleeing one is quicker to slip away).
        ShinerSchool* bs = nullptr;
        size_t bi = 0;
        float bd = 1e9f;
        for (ShinerSchool& sc : shinerSchools_) {
            if (!sc.live) continue;
            for (size_t i = 0; i < sc.fish.size(); ++i) {
                const float d = length(sc.fish[i].pos - hp);
                const float r = sc.fish[i].flee > 0 ? 0.22f : 0.32f;
                if (d < r && d < bd) { bd = d; bs = &sc; bi = i; }
            }
        }
        if (!bs) continue;
        bs->fish.erase(bs->fish.begin() + long(bi));
        hf = HeldFish{};
        hf.active = true;
        hf.prev = c.gripPose.position;
        hf.lastPos = hp;
        haptics_.push_back({h, 0.5f, 0.08f});
        sfx("bi_watr1", hp, 0.4f, 1.5f);
        LOGI("Fish: caught by hand %d", h);
    }
    // Loose fish: fall; into the water - back to the nearest school; on land - flop, then lie.
    auto ground = [&](const Vec3& p) { return player_.groundHeight ? player_.groundHeight(p.x, p.z) : sea - 1.0f; };
    for (LooseFish& lf : looseFish_) {
        lf.t += dt;
        if (!lf.landed) {
            const Vec3 was = lf.pos;
            lf.vel.y -= 9.8f * dt;
            lf.pos = lf.pos + lf.vel * dt;
            lf.yaw += lf.spin * dt;
            lf.roll += lf.spin * 0.5f * dt;
            const float speed = length(lf.vel);
            if (!lf.hit && speed > 5.0f) {
                const Vec3 dir = lf.vel * (1.0f / speed);
                for (size_t k = 0; k < enemies_.enemies.size(); ++k) {
                    const Enemy& e = enemies_.enemies[k];
                    if (e.state == EnemyState::Dead) continue;
                    const float top = enemies_.bodyHeight(e);
                    if (std::hypot(lf.pos.x - e.pos.x, lf.pos.z - e.pos.z) > 0.5f || lf.pos.y < e.pos.y || lf.pos.y > e.pos.y + top) continue;
                    lf.hit = true;
                    thrownHitEnemy(int(k), 1.0f + 0.3f * speed, lf.pos, dir);
                    sfx("bi_hit1", lf.pos, 0.7f, 1.5f);
                    lf.vel = lf.vel * -0.2f;
                    LOGI("Fish: a thrown fish slaps Reaper %u at %.1f m/s", e.uid, speed);
                    break;
                }
            }
            if (was.y >= sea && lf.pos.y < sea && ground(lf.pos) < sea - 0.3f) {
                waterSplash(Vec3{lf.pos.x, sea, lf.pos.z}, 0.15f, 1);
                LOGI("Fish: back into the water");
                lf.t = -1.0f;  // (swims off)
                continue;
            }
            if (lf.pos.y < sea - 0.1f && ground(lf.pos) < sea - 0.3f) { lf.t = -1.0f; continue; }  // (let go under the water)
            const float gy = ground(lf.pos);
            if (lf.pos.y <= gy) {
                lf.pos.y = gy;
                lf.landed = true;
                LOGI("Fish: lands on the ground - it flops");
                lf.vel = Vec3{};
                lf.roll = 1.5708f;
                lf.hopT = 0.3f;
            }
        } else if (lf.t < 8.0f) {
            // Flopping: little hops and twists, weaker and weaker.
            lf.hopT -= dt;
            if (lf.hopT <= 0) {
                lf.hopT = 0.35f + 0.6f * frand() + lf.t * 0.1f;
                lf.vel = Vec3{(frand() - 0.5f) * 0.8f, 1.2f + 1.0f * frand() * (1.0f - lf.t / 8.0f), (frand() - 0.5f) * 0.8f};
                lf.spin = (frand() - 0.5f) * 10.0f;
                sfx("bi_hit1", lf.pos, 0.15f, 2.2f);
            }
            lf.vel.y -= 9.8f * dt;
            lf.pos = lf.pos + lf.vel * dt;
            lf.yaw += lf.spin * dt;
            lf.roll = 1.5708f + std::sin(lf.t * 14.0f) * 0.4f * std::max(0.0f, 1.0f - lf.t / 8.0f);
            const float gy = ground(lf.pos);
            if (lf.pos.y < gy) { lf.pos.y = gy; lf.vel = Vec3{}; lf.spin *= 0.3f; }
            if (gy < sea - 0.2f) lf.t = -1.0f;  // (flopped back into the water)
        }
    }
    // The ones back in the water join the nearest school, darting at first.
    for (const LooseFish& lf : looseFish_) {
        if (lf.t >= 0) continue;
        ShinerSchool* best = nullptr;
        float bd = 1e9f;
        for (ShinerSchool& sc : shinerSchools_)
            if (sc.live && length(sc.centre - lf.pos) < bd) { bd = length(sc.centre - lf.pos); best = &sc; }
        if (!best || bd > 60.0f) continue;
        ShinerFish f;
        f.pos = Vec3{lf.pos.x, std::min(lf.pos.y, sea - 0.2f), lf.pos.z};
        f.offset = Vec3{(frand() - 0.5f) * 2.4f, -frand() * 0.25f, (frand() - 0.5f) * 2.4f};
        f.flee = 1.5f;
        Vec3 away{f.pos.x - player_.position.x, 0, f.pos.z - player_.position.z};
        f.vel = length(away) > 0.01f ? normalize(away) * 3.5f : Vec3{3.5f, 0, 0};
        best->fish.push_back(f);
    }
    looseFish_.erase(std::remove_if(looseFish_.begin(), looseFish_.end(), [](const LooseFish& f) { return f.t < 0 || f.t > 40.0f; }),
                     looseFish_.end());
}

}  // namespace gvr
