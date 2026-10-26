// GameApp: Smartie villagers (VillagerDirector + the original Smartie model and sm_* clips).
#include "game/game_app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

#include "core/log.h"
#include "game/enemy.h"
#include "scene/asset_world.h"
#include "scene/model_cache.h"
#include "scene/skinned_model.h"

namespace gvr {

void GameApp::initVillagers(AssetWorld* w, ModelCache* mc) {
    villagers_ = VillagerDirector{};
    villagerModels_.clear();
    villagerLift_.clear();
    if (!w || !w->hasTerrain()) return;
    // Villages: clusters of Smartie houses (smw_house*) within 60 m of each other.
    struct House { Vec3 c; float r; };
    std::vector<House> houses;
    for (const auto& o : w->worldObjects())
        if (o.name.rfind("smw_house", 0) == 0) houses.push_back({o.centre, o.radius});
    if (houses.size() < 3) return;
    std::vector<int> cluster(houses.size(), -1);
    int clusters = 0;
    for (size_t i = 0; i < houses.size(); ++i) {
        if (cluster[i] >= 0) continue;
        cluster[i] = clusters;
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t j = 0; j < houses.size(); ++j)
                if (cluster[j] < 0)
                    for (size_t k = 0; k < houses.size(); ++k)
                        if (cluster[k] == clusters && length(Vec3{houses[j].c.x - houses[k].c.x, 0, houses[j].c.z - houses[k].c.z}) < 60.0f) {
                            cluster[j] = clusters;
                            grew = true;
                            break;
                        }
        }
        ++clusters;
    }
    villagers_.groundHeight = [w](float x, float z) {
        float y;
        return w->terrain().heightAt(x, z, y) ? y : 0.0f;
    };
    villagers_.blocked = [w](const Vec3& a, const Vec3& b) {
        const Vec3 o = a + Vec3{0, 0.5f, 0};
        Vec3 d = (b + Vec3{0, 0.5f, 0}) - o;
        const float l = length(d);
        if (l < 1e-3f) return false;
        float t;
        Vec3 n;
        return w->collision().raycast(o, d * (1.0f / l), l + 0.3f, t, n);
    };
    static const char* clips[int(VillagerClip::Count)] = {"sm_stand.anm",   "sm_hwalk.anm",  "sm_scan.anm",      "sm_talk1.anm",
                                                          "sm_listen1.anm", "sm_laugh1.anm", "sm_stand_sing.anm", "sm_tai_chi.anm",
                                                          "sm_sit.anm",     "sm_celebrate.anm", "sm_wave.anm",   "sm_Run_terror.anm",
                                                          "sm_cower.anm",
                                                          // rework: hit, killed, held, a gun pointed at them
                                                          "sm_gets_shot.anm", "sm_death.anm", "sm_squirm.anm", "sm_hands_up.anm"};
    const int kMax = 16;
    for (int c = 0; c < clusters && int(villagers_.villagers.size()) < kMax; ++c) {
        Vec3 centre;
        int n = 0;
        float extent = 0;
        for (size_t i = 0; i < houses.size(); ++i)
            if (cluster[i] == c) { centre = centre + houses[i].c; ++n; }
        if (n < 3) continue;
        centre = centre * (1.0f / float(n));
        for (size_t i = 0; i < houses.size(); ++i)
            if (cluster[i] == c) extent = std::max(extent, length(Vec3{houses[i].c.x - centre.x, 0, houses[i].c.z - centre.z}));
        const int want = std::min(8, n + 2);
        int made = 0;
        for (int tries = 0; tries < 60 && made < want && int(villagers_.villagers.size()) < kMax; ++tries) {
            const float a = frand() * 6.2832f, d = 3.0f + frand() * (extent + 6.0f);
            Vec3 p = centre + Vec3{std::cos(a) * d, 0, std::sin(a) * d};
            float gy;
            if (!w->terrain().heightAt(p.x, p.z, gy) || gy < 0.4f) continue;  // on land
            p.y = gy;
            bool inside = false;
            for (const auto& h : houses) inside |= length(Vec3{p.x - h.c.x, 0, p.z - h.c.z}) < h.r * 0.85f;
            if (inside) continue;
            auto m = std::make_unique<SkinnedModel>();
            if (!m->load(w->fs(), *mc, renderer_, "sm_L0", clips[0])) return;
            if (villagerModels_.empty()) {
                for (int k = 1; k < int(VillagerClip::Count); ++k) villagerClip_[k] = std::max(0, m->addAnimation(w->fs(), clips[k]));
                for (int k = 0; k < int(VillagerClip::Count); ++k) villagers_.clipDuration[k] = m->animDuration(villagerClip_[k]);
            }
            else
                for (int k = 1; k < int(VillagerClip::Count); ++k) m->addAnimation(w->fs(), clips[k]);
            m->pose(renderer_, 0.0f);
            Vec3 mn, mx;
            m->posedBounds(mn, mx);
            const int i = villagers_.add(p, frand() * 6.2832f);
            villagers_.villagers[size_t(i)].home = centre + (p - centre) * 0.6f;
            villagers_.villagers[size_t(i)].home.y = p.y;
            villagers_.params.homeRadius = std::max(12.0f, extent + 4.0f);
            villagers_.villagers[size_t(i)].slot = int(villagerModels_.size());
            villagerModels_.push_back(std::move(m));
            villagerLift_.push_back(-mn.y);
            ++made;
        }
        LOGI("Village %d: %d houses, %d Smarties about (game %.0f, %.0f)", c, n, made, -centre.x / gGameUnitsToMeters,
             centre.z / gGameUnitsToMeters);
    }
}

void GameApp::updateVillagers(float dt) {
    if (villagers_.villagers.empty()) return;
    std::vector<Vec3> threats, scares;
    for (const auto& e : enemies_.enemies)
        if (e.state != EnemyState::Dead) threats.push_back(e.pos);
    for (const auto& c : creatures_.creatures)
        if (c.state != CreatureState::Dead && !CreatureDirector::underground(c)) threats.push_back(c.pos);
    if (hero_ == Hero::Kabuto) threats.push_back(player_.position);  // Smarties are his food
    // Rework: kill one of them and the rest are afraid of you for a while - they run from you, and
    // nobody waves.
    villagerFearT_ = std::max(0.0f, villagerFearT_ - dt);
    if (villagerFearT_ > 0 && hero_ != Hero::Kabuto) threats.push_back(player_.position);
    // ...and things flying at them frighten them: a thrown body, a stone, one of their own thrown.
    if (thrownBody_.kind >= 0)
        if (Ragdoll* r = bodyRagdoll(thrownBody_)) scares.push_back(r->centre());
    for (const auto& st : stones_)
        if (st.flying && length(st.vel) > 4.0f) scares.push_back(st.pos);
    for (const auto& f : flyingVillagers_) scares.push_back(f.pos);
    for (const auto& b : blasts_)
        if (b.age < 0.05f) scares.push_back(b.pos);
    if (weapon_.shotsFired() != villagerShots_) {  // the Meccaryn's gunfire frightens them too
        villagerShots_ = weapon_.shotsFired();
        scares.push_back(player_.position);
    }
    // Rework: when the last Reaper or Ripper near the village falls, the Smarties cheer (not for Kabuto).
    bool threatNear = false;
    for (const auto& t : threats)
        for (const auto& v : villagers_.villagers)
            if (!threatNear && length(Vec3{t.x - v.pos.x, 0, t.z - v.pos.z}) < 45.0f) threatNear = true;
    if (villageThreatWas_ && !threatNear && threats.size() < villageThreats_ && hero_ != Hero::Kabuto && !dying_) {
        const int n = villagers_.cheer();
        if (n > 0) {
            LOGI("The danger is over: %d Smarties cheer", n);
            float best = 1e9f;
            Vec3 at = player_.position;
            for (const auto& v : villagers_.villagers)
                if (length(v.pos - player_.position) < best) { best = length(v.pos - player_.position); at = v.pos; }
            if (best < 60.0f) {  // the nearest calls out
                static const int voices[] = {1, 2, 3, 4, 13, 14};
                char nm[16];
                std::snprintf(nm, sizeof nm, "gsm%02d_%d", voices[int(frand() * 5.99f)], frand() < 0.5f ? 1 : 2);
                sfx(nm, at + Vec3{0, 0.8f, 0}, 1.0f, 1.05f + 0.1f * frand());
            }
        }
    }
    villageThreatWas_ = threatNear;
    villageThreats_ = threats.size();
    villagers_.update(dt, player_.position, threats, scares);
    for (const auto& v : villagers_.villagers) {
        if (v.slot < 0) continue;
        const float d = length(v.pos - player_.position);
        if (d > 70.0f || (d > 25.0f && ((enemyPoseTick_ + uint32_t(v.slot)) & 1))) continue;  // beyond 25 m: every other frame
        const int clip = villagerClip_[int(v.clip)];
        villagerModels_[size_t(v.slot)]->poseBlend(renderer_, clip, v.clipTime, clip, v.clipTime, 0.0f);
    }
}

void GameApp::renderVillagers(bool shadow, const Vec3& eye, float range) {
    for (const auto& v : villagers_.villagers) {
        if (v.slot < 0 || length(v.pos - eye) > range) continue;
        const Mat4 xf = Mat4::translation(v.pos + Vec3{0, villagerLift_[size_t(v.slot)], 0}) * Mat4::rotation(Quat::fromAxisAngle({0, 1, 0}, v.yaw));
        if (shadow) villagerModels_[size_t(v.slot)]->renderShadow(renderer_, xf);
        else villagerModels_[size_t(v.slot)]->render(renderer_, xf);
    }
}

bool GameApp::villagerRay(const Vec3& o, const Vec3& d, float maxD, float& t, int& index) const {
    bool hit = false;
    float best = maxD;
    for (size_t i = 0; i < villagers_.villagers.size(); ++i) {
        const Villager& v = villagers_.villagers[i];
        if (!VillagerDirector::alive(v)) continue;
        float th;
        if (rayCapsule(o, d, maxD, v.pos + Vec3{0, 0.25f, 0}, v.pos + Vec3{0, 0.6f, 0}, 0.28f, th) && th < best) {
            best = th;
            index = int(i);
            hit = true;
        }
    }
    if (hit) t = best;
    return hit;
}

void GameApp::hitVillager(int i, float amount, const Vec3& at, const Vec3& dir) {
    if (i < 0 || size_t(i) >= villagers_.villagers.size()) return;
    Villager& v = villagers_.villagers[size_t(i)];
    if (!VillagerDirector::alive(v)) return;
    const bool killed = villagers_.damage(i, amount, at - dir * 5.0f);
    bleed(at, dir, killed, 0.6f);
    if (killed) blood_.pool(v.pos, 0.6f);
    // A cry (a Smartie voice, pitched up).
    static const int voices[] = {1, 2, 3, 4, 13, 14};
    char nm[16];
    std::snprintf(nm, sizeof nm, "gsm%02d_%d", voices[int(frand() * 5.99f)], frand() < 0.5f ? 1 : 2);
    sfx(nm, v.pos + Vec3{0, 0.6f, 0}, 1.0f, killed ? 0.8f : 1.3f);
    LOGI("Smartie %u %s", v.uid, killed ? "killed" : "hurt");
    if (killed && hero_ != Hero::Kabuto) {
        if (villagerFearT_ <= 0) LOGI("The Smarties are afraid of you now");
        villagerFearT_ = 40.0f;
    }
    if (killed && hero_ != Hero::Kabuto && !smartieWarned_) {
        smartieWarned_ = true;
        timmyLine_ = "HEY! THE SMARTIES ARE ON YOUR SIDE!";
        timmyLineT_ = 3.5f;
    }
}

// The Meccaryn and Delphi pick Smarties up too (rework): squeeze a grip with the hand on one (a
// squirming Smartie in your hand); open it gently to set it down, throw it and it flies - a hard
// landing kills it. A gun or bow held on one for a moment: it puts its hands up.
void GameApp::updateVillagerHands(float dt, const InputFrame& in) {
    const bool can = playMode_ && firstPerson_ && !dying_ && hero_ != Hero::Kabuto && !cutsceneActive();
    auto indexOf = [&](uint32_t uid) {
        for (size_t i = 0; i < villagers_.villagers.size(); ++i)
            if (villagers_.villagers[i].uid == uid) return int(i);
        return -1;
    };
    for (int h = 0; h < 2; ++h) {
        const ControllerState& c = in.hand[h];
        const bool g = c.squeeze > 0.7f;
        const bool edge = g && !villagerGripWas_[h];
        villagerGripWas_[h] = g;
        if (!can || !edge || heldVillager_ != 0 || !c.gripPoseValid || heldBody_.hand == h || stoneInHand(h) || climbHold_[h]) continue;
        const Vec3 hp = rig_.toWorld(c.gripPose).position, p = reachPoint(hp);  // (GROUND REACH)
        static const bool gtrace = std::getenv("GVR_GRAB_TRACE") != nullptr;
        if (gtrace) {
            float nd = 1e9f;
            Vec3 at;
            for (const auto& v : villagers_.villagers)
                if (VillagerDirector::alive(v) && length(v.pos - hp) < nd) { nd = length(v.pos - hp); at = v.pos; }
            LOGI("grab trace: hand %d at (%.2f %.2f %.2f), nearest Smartie %.2f m at (%.2f %.2f %.2f) = game (%.0f, %.0f)", h, hp.x, hp.y, hp.z, nd, at.x, at.y, at.z, -at.x / 0.2f, at.z / 0.2f);
        }
        for (const auto& v : villagers_.villagers) {
            if (!VillagerDirector::alive(v) || std::min(length(hp - (v.pos + Vec3{0, 0.45f, 0})), length(p - (v.pos + Vec3{0, 0.45f, 0}))) > 0.45f) continue;
            heldVillager_ = v.uid;
            heldVillagerHand_ = h;
            heldVillagerPrev_ = c.gripPose.position;  // (tracking space: handVelocity)
            heldVillagerVel_ = Vec3{0, 0, 0};
            heldVillagerN_ = 0;
            villagers_.hold(indexOf(v.uid), true);
            haptics_.push_back({h, 0.5f, 0.08f});
            LOGI("Picked up Smartie %u (hand %d)", v.uid, h);
            break;
        }
    }
    if (heldVillager_ != 0) {
        const int i = indexOf(heldVillager_);
        const ControllerState& c = in.hand[heldVillagerHand_ < 0 ? 0 : heldVillagerHand_];
        if (i < 0 || !VillagerDirector::alive(villagers_.villagers[size_t(i)]) || !can || !c.gripPoseValid) {
            heldVillager_ = 0;
        } else {
            const Pose gp = rig_.toWorld(c.gripPose);
            const Vec3 p = gp.position;
            if (dt > 0) {
                const Vec3 hv = handVelocity(c.gripPose.position, heldVillagerPrev_, dt);
                heldVillagerVel_ = heldVillagerVel_ * 0.5f + hv * 0.5f;
                heldVillagerRecent_[heldVillagerN_++ % 12] = hv;
            }
            Villager& v = villagers_.villagers[size_t(i)];
            v.pos = p - Vec3{0, 0.45f, 0};
            v.yaw = yawOf(gp.orientation);
            if (frand() < dt * 1.5f) haptics_.push_back({heldVillagerHand_, 0.2f, 0.05f});  // it wriggles
            if (c.squeeze < 0.35f) {
                Vec3 thr = heldVillagerVel_;
                for (int k = 0; k < std::min(heldVillagerN_, 12); ++k)
                    if (length(heldVillagerRecent_[k]) > length(thr)) thr = heldVillagerRecent_[k];
                if (length(thr) > 2.5f) {
                    flyingVillagers_.push_back({v.uid, v.pos, thr * 1.2f});
                    LOGI("Threw Smartie %u (%.1f m/s)", v.uid, length(thr) * 1.2f);
                } else {
                    float gy = v.pos.y;
                    if (villagers_.groundHeight) gy = villagers_.groundHeight(v.pos.x, v.pos.z);
                    flyingVillagers_.push_back({v.uid, v.pos, Vec3{0, 0, 0}});  // (drops the little way down)
                    (void)gy;
                    LOGI("Set Smartie %u down", v.uid);
                }
                heldVillager_ = 0;
            }
        }
    }
    // Thrown or dropped: falls; a hard landing (or a flight into something) kills it.
    for (auto& f : flyingVillagers_) {
        const int i = indexOf(f.uid);
        if (i < 0) { f.uid = 0; continue; }
        Villager& v = villagers_.villagers[size_t(i)];
        f.vel.y -= 14.0f * dt;
        f.pos = f.pos + f.vel * dt;
        const float g = villagers_.groundHeight ? villagers_.groundHeight(f.pos.x, f.pos.z) : f.pos.y - 1.0f;
        v.pos = f.pos;
        static const bool ftrace = std::getenv("GVR_GRAB_TRACE") != nullptr;
        if (ftrace) {
            float nd = 1e9f;
            Vec3 at;
            for (const auto& e : enemies_.enemies)
                if (e.state != EnemyState::Dead && length(e.pos - f.pos) < nd) { nd = length(e.pos - f.pos); at = e.pos; }
            LOGI("grab trace: Smartie in flight at (%.2f %.2f %.2f) %.1f m/s, nearest Reaper %.2f m at (%.2f %.2f %.2f)", f.pos.x, f.pos.y, f.pos.z, length(f.vel), nd, at.x, at.y, at.z);
        }
        // Thrown into a Reaper: it bowls him back and hurts him (and the Smartie drops, stunned - or dead).
        if (length(f.vel) > 5.0f && !f.hit)
            for (size_t k = 0; k < enemies_.enemies.size(); ++k) {
                const Enemy& e = enemies_.enemies[k];
                if (e.state == EnemyState::Dead || std::hypot(e.pos.x - f.pos.x, e.pos.z - f.pos.z) > 0.6f || f.pos.y < e.pos.y - 0.2f ||
                    f.pos.y > e.pos.y + enemies_.bodyHeight(e))
                    continue;
                const float sp = length(f.vel);
                const Vec3 dir = normalize(Vec3{f.vel.x, 0, f.vel.z} + Vec3{0.001f, 0, 0});
                enemies_.push(int(k), dir * std::min(6.0f, 0.8f * sp));
                const bool killed = thrownHitEnemy(int(k), 5.0f + 2.5f * sp, f.pos, dir);
                sfx(frand() < 0.5f ? "ex_bdys1" : "ex_bdys2", f.pos, 0.9f, 1.1f);
                LOGI("A thrown Smartie hits Reaper %u at %.1f m/s%s", e.uid, sp, killed ? " - he's down" : "");
                f.vel = Vec3{-f.vel.x * 0.2f, std::max(1.0f, f.vel.y), -f.vel.z * 0.2f};  // (it bounces off him)
                f.hit = true;
                break;
            }
        if (f.pos.y <= g) {
            v.pos.y = g;
            const float impact = length(f.vel);
            if (impact > 9.0f) hitVillager(i, 100.0f, v.pos + Vec3{0, 0.3f, 0}, normalize(f.vel));
            else villagers_.hold(i, false);  // on its feet again: cowering
            f.uid = 0;
        }
    }
    flyingVillagers_.erase(std::remove_if(flyingVillagers_.begin(), flyingVillagers_.end(), [](const FlyingVillager& f) { return f.uid == 0; }),
                           flyingVillagers_.end());
    // A gun (or bow) pointed at one for a moment: hands up.
    if (can && (gunValid_ || bowValid_) && in.hand[rig_.turnHand()].aimPoseValid) {
        const Pose a = rig_.toWorld(in.hand[bowValid_ && hero_ == Hero::Delphi ? rig_.moveHand() : rig_.turnHand()].aimPose);
        const Vec3 f = a.orientation.rotate(Vec3{0, 0, -1});
        uint32_t best = 0;
        float bestC = 0.995f;
        for (const auto& v : villagers_.villagers) {
            if (!VillagerDirector::alive(v) || v.state == VillagerState::Held) continue;
            const Vec3 to = v.pos + Vec3{0, 0.45f, 0} - a.position;
            const float d = length(to);
            if (d > 25.0f || d < 0.5f) continue;
            const float cs = dot(to * (1.0f / d), f);
            if (cs > bestC) { bestC = cs; best = v.uid; }
        }
        aimedVillagerT_ = best != 0 && best == aimedVillager_ ? aimedVillagerT_ + dt : 0.0f;
        aimedVillager_ = best;
        if (aimedVillagerT_ > 0.4f) {
            const int i = indexOf(best);
            if (i >= 0 && villagers_.villagers[size_t(i)].state != VillagerState::HandsUp) {
                villagers_.handsUp(i);
                LOGI("Smartie %u puts its hands up", best);
            }
        }
    }
}

}  // namespace gvr
